#include "bybit_broker.hpp"

#include "live_debug.hpp"
#include "logger.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

namespace {

// bybit v5 drops a private socket that stays silent for ~30s, so ping well
// under it. the loop sleeps in slices so Stop() is not blocked for a full
// interval.
constexpr int kHeartbeatIntervalMs = 20000;
constexpr int kHeartbeatSliceMs = 100;

// a market entry is echoed (fill or reject) far inside this window. a pending
// flag older than this is a lost push, not a live order.
constexpr int64_t kPendingEntryTimeoutMs = 10000;

// bybit mixes strings and numbers for the same field across endpoints.
double JsonNum(const nlohmann::json& j, const char* key, double fallback = 0.0) {
    if (!j.contains(key)) return fallback;
    const auto& v = j[key];
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        try {
            return std::stod(v.get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

// lowercase hex encoding of a raw HMAC digest
std::string ToHex(const unsigned char* data, size_t len) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0F]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

int64_t NowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// libcurl writes the response body through this callback; `userp` is the
// caller-owned std::string we accumulate into.
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t total = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
    return total;
}

// Formats a price/qty for a JSON body. Bybit rejects values with trailing
// zeros, so this emits the full value with no padding. setprecision(15) keeps
// enough significant digits to stay on the tick/step grid -- the ostream
// default of 6 would silently round 65000.123456 down to 65000.1.
std::string FormatDouble(double value) {
    std::ostringstream oss;
    oss << std::setprecision(15) << value;
    return oss.str();
}

// Like FormatDouble, but emitted with exactly the number of decimals the
// venue's step implies. floor(qty / step) * step lands on values such as
// 0.0009999999999999998, which a 15-digit dump sends verbatim and the venue
// then rejects as off-grid. Formatting to the step's own precision makes the
// wire value equal the intended grid point.
std::string FormatStep(double value, double step) {
    int decimals = 0;
    if (step > 0.0) {
        double scaled = step;
        while (decimals < 8 && std::abs(scaled - std::round(scaled)) > 1e-12) {
            scaled *= 10.0;
            ++decimals;
        }
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(decimals) << value;
    std::string out = oss.str();
    if (decimals > 0) {
        while (!out.empty() && out.back() == '0') out.pop_back();
        if (!out.empty() && out.back() == '.') out.pop_back();
    }
    return out;
}

// rounds a price or qty to the venue's grid (tickSize for price, qtyStep for
// size). bybit rejects a value that is not a multiple of the step, and a
// rejected break-even move leaves the position at its previous stop while
// the local ladder believes it moved.
double RoundToStep(double value, double step) {
    if (!(step > 0.0)) return value;
    return std::round(value / step) * step;
}

} // namespace

int64_t BybitBroker::NowSignedMillis() const {
    return NowMillis() + clock_offset_ms_;
}

BybitBroker::BybitBroker(const std::string& api_key,
                         const std::string& api_secret,
                         mokuren::venue::Spec spec)
    : api_key_(api_key), api_secret_(api_secret), spec_(spec) {
    // every host comes from the resolved spec, so the broker and the
    // market-data streamer can never disagree about mainnet/testnet/demo.
    rest_base_url_  = spec_.rest_base;
    private_ws_url_ = spec_.private_ws;

    LOG_INFO("BybitBroker: venue {}", mokuren::venue::Manifest(spec_));

    // before anything can trade: is the clock usable, what does this symbol
    // accept, and is the account already holding something we did not open?
    EstablishStartupTruth();

    ConnectPrivateStream();
}

void BybitBroker::EstablishStartupTruth() {
    // time first. every signed call below depends on it, and a wrong clock
    // turns a valid key into "retCode 10004 invalid signature".
    SyncClock();

    // fail fast: without the venue's own step and minimums, ProcessSignal is
    // guessing, and a guessed quantity is a guaranteed reject (or worse, a
    // silently floored order the strategy did not intend).
    if (!FetchInstrumentRules()) {
        throw std::runtime_error(
            "BybitBroker: could not read instrument rules for " + spec_.symbol +
            " -- refusing to trade blind");
    }

    // a prop account that restarts with an open position must adopt it, not
    // open a second one. this also fixes HasOpenPosition() for the window
    // before the first private push arrives.
    ReconcileAccount();
}

void BybitBroker::SyncClock() {
    nlohmann::json j;
    if (!SendRestGet("/v5/market/time", "", j)) {
        throw std::runtime_error(
            "BybitBroker: cannot read server time; refusing to sign requests "
            "against an unverified clock");
    }

    int64_t server_ms = 0;
    if (j.contains("result") && j["result"].is_object()) {
        const auto& res = j["result"];
        if (res.contains("timeNano")) {
            server_ms = static_cast<int64_t>(JsonNum(res, "timeNano") / 1e6);
        } else if (res.contains("timeSecond")) {
            server_ms = static_cast<int64_t>(JsonNum(res, "timeSecond") * 1000.0);
        }
    }
    if (server_ms <= 0) {
        throw std::runtime_error("BybitBroker: server time response carried no "
                                 "usable timestamp");
    }

    clock_offset_ms_ = server_ms - NowMillis();
    LOG_INFO("BybitBroker: clock offset {} ms", clock_offset_ms_);

    // every signed request is stamped with the corrected clock, so a large
    // offset is not fatal -- it would only become one if a caller used
    // NowMillis() uncorrected. surface it anyway: a host that is seconds off
    // is a broken ntp setup, and the operator should know before it becomes
    // a "why did every order reject overnight" thread.
    if (clock_offset_ms_ > 1000 || clock_offset_ms_ < -1000) {
        LOG_WARN("BybitBroker: local clock is off by {} ms; signatures are "
                 "corrected, but check the host's time sync", clock_offset_ms_);
    }
}

bool BybitBroker::FetchInstrumentRules() {
    const std::string query =
        "category=" + spec_.category + "&symbol=" + spec_.symbol;

    nlohmann::json j;
    if (!SendRestGet("/v5/market/instruments-info", query, j)) return false;
    if (!j.contains("result") || !j["result"].is_object() ||
        !j["result"].contains("list") || !j["result"]["list"].is_array() ||
        j["result"]["list"].empty()) {
        return false;
    }

    const auto& item = j["result"]["list"][0];
    InstrumentRules r;
    if (item.contains("priceFilter")) {
        r.tick_size = JsonNum(item["priceFilter"], "tickSize");
    }
    if (item.contains("lotSizeFilter")) {
        const auto& lot = item["lotSizeFilter"];
        r.qty_step = JsonNum(lot, "qtyStep");
        r.min_qty = JsonNum(lot, "minOrderQty");
        r.min_notional = JsonNum(lot, "minNotionalValue");
    }
    if (item.contains("leverageFilter")) {
        r.max_leverage = JsonNum(item["leverageFilter"], "maxLeverage");
    }

    if (!(r.qty_step > 0.0)) {
        LOG_ERROR("BybitBroker: instruments-info reported no qtyStep for {}",
                  spec_.symbol);
        return false;
    }

    r.valid = true;
    rules_ = r;

    LOG_INFO("BybitBroker: {} tick={} step={} minQty={} minNotional={} maxLev={}",
             spec_.symbol, rules_.tick_size, rules_.qty_step, rules_.min_qty,
             rules_.min_notional, rules_.max_leverage);
    return true;
}

void BybitBroker::ReconcileAccount() {
    const std::string query =
        "category=" + spec_.category + "&symbol=" + spec_.symbol;

    nlohmann::json j;
    if (!SendRestGet("/v5/position/list", query, j)) {
        LOG_ERROR("BybitBroker: could not read open positions at startup; the "
                  "local position state starts empty");
        return;
    }

    if (!j.contains("result") || !j["result"].is_object() ||
        !j["result"].contains("list") || !j["result"]["list"].is_array()) {
        return;
    }

    for (const auto& p : j["result"]["list"]) {
        if (!p.is_object()) continue;
        const double size = JsonNum(p, "size");
        if (!(size > 0.0)) continue;

        std::lock_guard<std::mutex> lock(state_mutex_);
        position_size_ = size;
        entry_price_ = JsonNum(p, "entryPrice");
        current_sl_ = JsonNum(p, "stopLoss");
        current_tp_ = JsonNum(p, "takeProfit");
        current_position_.direction =
            (p.value("side", "") == "Buy") ? SignalDirection::BUY
                                           : SignalDirection::SELL;
        current_position_.volume = size;
        current_position_.entry_price = entry_price_;
        current_position_.stop_loss = current_sl_;
        current_position_.take_profit = current_tp_;

        // adopt, never flatten: liquidating someone else's position on startup
        // is not the engine's decision to make, but re-entering on top of it
        // would breach every single-position rule the prop account has.
        LOG_WARN("BybitBroker: adopted an existing {} {} position (size={} "
                 "entry={} sl={} tp={}); the strategy will manage it as its own",
                 p.value("side", "?"), spec_.symbol, size, entry_price_,
                 current_sl_, current_tp_);
    }
}

BybitBroker::~BybitBroker() {
    // joins the heartbeat and websocket threads before members are destroyed.
    Stop();
}

void BybitBroker::ConnectPrivateStream() {
    private_ws_.setUrl(private_ws_url_);

    // stale account state is dangerous -- the engine would size the next order
    // against a position that no longer exists -- so reconnect automatically.
    // Open fires again on every reconnect, which re-runs the handshake and the
    // subscriptions below. setPingInterval is protocol-level keepalive;
    // HeartbeatLoop covers the application-level {"op":"ping"} Bybit also wants.
    private_ws_.enableAutomaticReconnection();
    private_ws_.setPingInterval(20);

    private_ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
            case ix::WebSocketMessageType::Open:
                // connection is up -> run the HMAC handshake. the subscription
                // is sent from the auth ack (see HandlePrivateMessage): bybit
                // rejects a subscribe that arrives before auth completes, and
                // sending both back to back raced that requirement.
                AuthenticatePrivateStream();
                ws_connected_ = true;
                break;
            case ix::WebSocketMessageType::Message:
                HandlePrivateMessage(msg->str);
                break;
            case ix::WebSocketMessageType::Error:
            case ix::WebSocketMessageType::Close:
                ws_connected_ = false;
                break;
            default:
                break;
        }
    });

    // start the heartbeat alongside the socket so it is alive as soon as the
    // handshake completes.
    keep_alive_.store(true, std::memory_order_release);
    heartbeat_thread_ = std::thread(&BybitBroker::HeartbeatLoop, this);

    private_ws_.start();
}

void BybitBroker::Stop() {
    // idempotent: exchange() returns false once the broker is already stopped.
    if (!keep_alive_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }

    private_ws_.stop();
    ws_connected_ = false;
}

void BybitBroker::HeartbeatLoop() {
    while (keep_alive_.load(std::memory_order_acquire)) {
        // sleep in small slices so Stop() is not blocked for a full interval.
        for (int waited = 0; waited < kHeartbeatIntervalMs;
             waited += kHeartbeatSliceMs) {
            if (!keep_alive_.load(std::memory_order_acquire)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(kHeartbeatSliceMs));
        }

        if (!keep_alive_.load(std::memory_order_acquire)) return;

        // only ping a live socket -- send() on a closed one is a no-op, and
        // logging a ping nobody receives would just be noise.
        if (ws_connected_.load(std::memory_order_acquire)) {
            LIVE_DBG("bybit: sending heartbeat ping");
            private_ws_.send(R"({"op":"ping"})");
        }
    }
}

void BybitBroker::AuthenticatePrivateStream() {
    // V5 private auth message:
    //   {"op":"auth","args":["<api_key>", <expires>, "<signature>"]}
    // signature = HMAC_SHA256(api_secret, "GET/realtime" + expires)
    const int64_t expires = NowSignedMillis() + 10000;
    const std::string signature =
        GenerateSignature("GET/realtime" + std::to_string(expires), "");

    const std::string auth_msg =
        "{\"op\":\"auth\",\"args\":[\"" + api_key_ + "\"," +
        std::to_string(expires) + ",\"" + signature + "\"]}";
    private_ws_.send(auth_msg);
}

void BybitBroker::SubscribePrivateTopics() {
    // position -> live size and SL/TP, execution -> fills and closedPnl,
    // wallet -> equity. wallet is not optional: the sizer multiplies equity by
    // risk_per_trade_pct, and without this subscription that number stayed 0.0
    // so no volume was ever produced.
    private_ws_.send(
        R"({"op":"subscribe","args":["position","execution","wallet"]})");
}

void BybitBroker::HandlePrivateMessage(const std::string& payload) {
    // parse without throwing: malformed frames from the exchange are dropped
    // instead of tearing down the websocket thread.
    const nlohmann::json j = nlohmann::json::parse(payload, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        LIVE_DBG("bybit: dropped unparsable private frame: {}", payload);
        return;
    }

    // the auth ack is the gate for the subscription. bybit discards a
    // subscribe that arrives before the handshake is confirmed, and a broker
    // that never subscribes reports equity 0.0 forever.
    const std::string op = j.value("op", "");
    if (op == "auth") {
        if (j.value("success", false)) {
            SubscribePrivateTopics();
        } else {
            LOG_ERROR("BybitBroker: private stream auth failed: {}",
                      j.value("retMsg", "no reason given"));
        }
        return;
    }
    if (op == "subscribe" && !j.value("success", true)) {
        LOG_ERROR("BybitBroker: private stream subscribe rejected: {}",
                  j.value("retMsg", "no reason given"));
        return;
    }

    const std::string topic = j.value("topic", "");
    if (topic != "position" && topic != "execution" && topic != "wallet") {
        // pongs and subscribe confirmations land here.
        return;
    }
    if (!j.contains("data") || !j["data"].is_array()) return;

    // One lock for the whole handler: the two topics are mutually exclusive,
    // and every field written below is guarded by state_mutex_.
    std::lock_guard<std::mutex> lock(state_mutex_);

    // Bybit mixes strings and numbers across versions of the same field, and
    // a malformed value must not crash the websocket thread.
    try {
        if (topic == "position") {
            for (const auto& entry : j["data"]) {
                if (!entry.is_object()) continue;
                // single-symbol broker: only the configured symbol matters.
                if (entry.value("symbol", "") != spec_.symbol) continue;

                position_size_ =
                    entry.contains("size") ? std::stod(entry["size"].get<std::string>()) : 0.0;

                // the exchange has now spoken about this symbol, filled or not:
                // the pending-entry guard can stand down.
                pending_entry_ = false;

                if (position_size_ > 0.0) {
                    entry_price_ = entry.contains("entryPrice")
                                       ? std::stod(entry["entryPrice"].get<std::string>())
                                       : 0.0;
                    current_sl_ = entry.contains("stopLoss")
                                      ? std::stod(entry["stopLoss"].get<std::string>())
                                      : 0.0;
                    current_tp_ = entry.contains("takeProfit")
                                      ? std::stod(entry["takeProfit"].get<std::string>())
                                      : 0.0;

                    const std::string side = entry.value("side", "");
                    current_position_.direction =
                        (side == "Buy") ? SignalDirection::BUY : SignalDirection::SELL;
                    current_position_.volume = position_size_;
                    current_position_.entry_price = entry_price_;
                    current_position_.stop_loss = current_sl_;
                    current_position_.take_profit = current_tp_;
                } else {
                    position_size_ = 0.0;
                    current_position_.direction = SignalDirection::NONE;
                    current_position_.volume = 0.0;
                }
            }
        } else if (topic == "wallet") {
            // uta wallet push. totalEquity is what the sizer multiplies.
            for (const auto& entry : j["data"]) {
                if (!entry.is_object()) continue;
                double equity = 0.0;
                if (entry.contains("totalEquity")) {
                    equity = JsonNum(entry, "totalEquity");
                } else if (entry.contains("list") && entry["list"].is_array()) {
                    for (const auto& coin : entry["list"]) {
                        if (coin.is_object()) equity += JsonNum(coin, "equity");
                    }
                }
                if (equity > 0.0) {
                    current_balance_ = equity;
                    LOG_INFO("BybitBroker: wallet equity {}", current_balance_);
                }
            }
        } else if (topic == "execution") {
            for (const auto& entry : j["data"]) {
                if (!entry.is_object()) continue;
                if (entry.value("symbol", "") != spec_.symbol) continue;

                // any execution on this symbol answers the pending entry.
                pending_entry_ = false;

                const double closed = JsonNum(entry, "closedSize");
                if (closed > 0.0) {
                    last_closed_size_ = closed;

                    // recorded so GetLastTradeTime/GetTradeCount work: without
                    // it AntiRevengeLock's cooldown never engaged and a risk
                    // module was silently switched off on the venue path.
                    TradeRecord rec{};
                    rec.entry_time = 0;
                    rec.exit_time =
                        static_cast<int64_t>(JsonNum(entry, "execTime"));
                    rec.direction = (entry.value("side", "") == "Buy")
                                        ? SignalDirection::BUY
                                        : SignalDirection::SELL;
                    rec.entry_price = entry_price_;
                    rec.exit_price = JsonNum(entry, "execPrice");
                    rec.net_profit = JsonNum(entry, "closedPnl");
                    rec.exit_reason = "venue_fill";
                    trade_history_.push_back(rec);
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("BybitBroker: bad numeric field in private '{}' frame: {}", topic, e.what());
    }
}

std::string BybitBroker::GenerateSignature(const std::string& payload,
                                           const std::string& timestamp) {
    // Bybit signs every authenticated request with the API secret. The exact
    // message differs per endpoint, so callers pass either the fully composed
    // signing string (REST) or the raw WS auth string. A non-empty `timestamp`
    // is prefixed to `payload`.
    const std::string message = timestamp.empty() ? payload : (timestamp + payload);

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    HMAC(EVP_sha256(),
         api_secret_.data(), static_cast<int>(api_secret_.size()),
         reinterpret_cast<const unsigned char*>(message.data()), message.size(),
         digest, &digest_len);

    return ToHex(digest, digest_len);
}

int BybitBroker::SendRestRequest(const std::string& method,
                                 const std::string& endpoint,
                                 const std::string& payload) {
    // Builds an authenticated V5 REST call. Required headers:
    //   X-BAPI-API-KEY, X-BAPI-TIMESTAMP, X-BAPI-RECV-WINDOW, X-BAPI-SIGN
    // where X-BAPI-SIGN = HMAC_SHA256(timestamp + api_key + recv_window + payload).
    const std::string timestamp = std::to_string(NowSignedMillis());
    const std::string recv_window = "5000";
    const std::string signing_string = timestamp + api_key_ + recv_window + payload;
    const std::string signature = GenerateSignature(signing_string, "");

    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("BybitBroker: curl_easy_init() failed for {}", endpoint);
        // -1 = transport failure, never 0 (accepted). callers must not treat
        // this as a submitted order.
        return -1;
    }

    const std::string url = rest_base_url_ + endpoint;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("X-BAPI-API-KEY: " + api_key_).c_str());
    headers = curl_slist_append(headers, ("X-BAPI-SIGN: " + signature).c_str());
    headers = curl_slist_append(headers, ("X-BAPI-TIMESTAMP: " + timestamp).c_str());
    headers = curl_slist_append(headers, ("X-BAPI-RECV-WINDOW: " + recv_window).c_str());

    std::string response_body;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    // NOTE: only POST bodies are wired up for now; signed GET requests carry
    // their parameters in the query string and need the payload appended to
    // the URL instead.
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

    int ret_code = -1;
    const CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        LOG_ERROR("BybitBroker: REST {} {} failed: {}", method, endpoint,
                  curl_easy_strerror(res));
    } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        LIVE_DBG("bybit: REST {} {} -> {} | REST Response: {}", method, endpoint,
                 http_code, response_body);

        if (http_code == 429) {
            LOG_ERROR("HTTP 429 Rate Limit hit! Bybit is blocking requests. Sleeping for 1 second.");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        } else if (http_code < 200 || http_code >= 300) {
            LOG_ERROR("BybitBroker: REST {} {} HTTP {}: {}", method, endpoint,
                      http_code, response_body);
        } else {
            // retCode 0 means the request was accepted. anything else is an
            // exchange-level reject (bad qty, wrong positionIdx, ...) and must
            // never be reported as a submitted order.
            const nlohmann::json j =
                nlohmann::json::parse(response_body, nullptr, false);
            if (!j.is_discarded() && j.is_object() && j.contains("retCode") &&
                j["retCode"].is_number_integer()) {
                ret_code = j["retCode"].get<int>();
            }
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ret_code;
}

bool BybitBroker::SendRestGet(const std::string& path, const std::string& query,
                              nlohmann::json& out) {
    // GET signs timestamp + api_key + recv_window + query string. the body is
    // empty, which is why this cannot reuse SendRestRequest's POSTFIELDS path.
    const std::string timestamp = std::to_string(NowSignedMillis());
    const std::string recv_window = "5000";
    const std::string signature = GenerateSignature(
        timestamp + api_key_ + recv_window + query, "");

    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("BybitBroker: curl_easy_init() failed for {}", path);
        return false;
    }

    const std::string url =
        rest_base_url_ + path + (query.empty() ? "" : "?" + query);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("X-BAPI-API-KEY: " + api_key_).c_str());
    headers = curl_slist_append(headers, ("X-BAPI-SIGN: " + signature).c_str());
    headers = curl_slist_append(headers, ("X-BAPI-TIMESTAMP: " + timestamp).c_str());
    headers = curl_slist_append(headers, ("X-BAPI-RECV-WINDOW: " + recv_window).c_str());

    std::string response_body;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    bool ok = false;
    const CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        LOG_ERROR("BybitBroker: GET {} failed: {}", path, curl_easy_strerror(res));
    } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        LIVE_DBG("bybit: GET {} -> {} | {}", path, http_code, response_body);

        out = nlohmann::json::parse(response_body, nullptr, false);
        if (out.is_discarded() || !out.is_object()) {
            LOG_ERROR("BybitBroker: GET {} returned an unparsable body", path);
        } else if (out.value("retCode", -1) != 0) {
            LOG_ERROR("BybitBroker: GET {} retCode {} {}", path,
                      out.value("retCode", -1), out.value("retMsg", ""));
        } else {
            ok = true;
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

// ---- cached state getters (served without blocking) ----

double BybitBroker::GetBalance() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_balance_;
}

bool BybitBroker::PendingEntryLocked() const {
    if (!pending_entry_) return false;
    if (NowMillis() - pending_since_ms_ > kPendingEntryTimeoutMs) {
        pending_entry_ = false;
        return false;
    }
    return true;
}

bool BybitBroker::HasOpenPosition() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    // a submitted-but-unconfirmed entry counts as open. the exchange echo
    // arrives tens of milliseconds later, and SinglePositionLock runs inside
    // that window; without this it admitted a second entry per burst.
    return position_size_ > 0.0 || PendingEntryLocked();
}

TradeSignal BybitBroker::GetCurrentPosition() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_position_;
}

double BybitBroker::GetNetProfit() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return net_profit_;
}

std::vector<TradeRecord> BybitBroker::GetTradeHistory() const {
    // the copy is taken while the lock is held, so the caller receives a
    // consistent snapshot instead of a reference into a vector the websocket
    // thread may be appending to.
    std::lock_guard<std::mutex> lock(state_mutex_);
    return trade_history_;
}

int64_t BybitBroker::GetLastTradeTime() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return trade_history_.empty() ? 0 : trade_history_.back().exit_time;
}

size_t BybitBroker::GetTradeCount() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return trade_history_.size();
}

double BybitBroker::GetLastClosedSize() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_closed_size_;
}

// ---- orchestration hooks ----

void BybitBroker::ProcessSignal(TradeSignal signal, double fill_price,
                                int64_t current_time) {
    if (signal.volume <= 0.0 || signal.direction == SignalDirection::NONE) return;

    // the venue, not the sizer, is the authority on what it accepts. the sizer
    // rounds to a coarse 0.001 step; the real step and minimums come from
    // instruments-info and are enforced here, so an off-grid or too-small
    // quantity is reported as a refusal instead of an exchange reject.
    double qty = signal.volume;
    if (rules_.valid && rules_.qty_step > 0.0) {
        qty = std::floor(qty / rules_.qty_step) * rules_.qty_step;
    }
    if (!(qty > 0.0)) {
        LOG_ERROR("BybitBroker: entry qty {} floors to zero on step {}; "
                  "refusing to send", signal.volume, rules_.qty_step);
        return;
    }
    if (rules_.valid) {
        if (rules_.min_qty > 0.0 && qty < rules_.min_qty) {
            LOG_ERROR("BybitBroker: entry qty {} is below minOrderQty {}; "
                      "refusing to send", qty, rules_.min_qty);
            return;
        }
        if (rules_.min_notional > 0.0 && fill_price > 0.0 &&
            qty * fill_price < rules_.min_notional) {
            LOG_ERROR("BybitBroker: entry notional {} is below the venue "
                      "minimum {}; refusing to send", qty * fill_price,
                      rules_.min_notional);
            return;
        }
    }

    // HYBRID APPROACH:
    // The entry order is submitted to POST /v5/order/create with the entry
    // parameters AND the protective levels attached in the same request:
    //
    //   {
    //     "category":    "linear",
    //     "symbol":      "BTCUSDT",
    //     "side":        "Buy" | "Sell",
    //     "orderType":   "Market",
    //     "qty":         "<signal.volume>",
    //     "stopLoss":    "<signal.stop_loss>",    // initial SL lives server-side
    //     "takeProfit":  "<signal.take_profit>",  // initial TP lives server-side
    //     "slTriggerBy": "LastPrice",
    //     "tpTriggerBy": "LastPrice"
    //   }
    //
    // This keeps the hard stop on Bybit's matching engine even if the local
    // process dies. Dynamic management (break-even / trailing) is layered on
    // top afterwards via CheckRisk -> UpdateStopLoss, which issues
    // POST /v5/position/trading-stop.
    //
    nlohmann::json payload = {
        {"category", spec_.category},
        {"symbol", spec_.symbol},
        {"orderType", "Market"},
        {"side", (signal.direction == SignalDirection::BUY) ? "Buy" : "Sell"},
        {"qty", FormatStep(qty, rules_.qty_step)},
        // a unique tag per intent. retries and restarts cannot be deduplicated
        // by the venue without one, and it is what makes an order traceable in
        // the bybit ui next to the engine log line.
        {"orderLinkId", "mok-" + std::to_string(NowSignedMillis()) + "-" +
                            std::to_string(++order_seq_)},
    };

    // Protective levels are attached to the entry so the hard stop lives on
    // Bybit's matching engine even if this process dies. Both prices are
    // rounded to the venue tick before they leave: an off-tick stop-loss is
    // rejected with the whole order, which would silently drop the protection.
    if (signal.stop_loss > 0.0) {
        const double sl = RoundToStep(signal.stop_loss, rules_.tick_size);
        payload["stopLoss"] = FormatStep(sl, rules_.tick_size);
        payload["slTriggerBy"] = "LastPrice";
    }
    if (signal.take_profit > 0.0) {
        const double tp = RoundToStep(signal.take_profit, rules_.tick_size);
        payload["takeProfit"] = FormatStep(tp, rules_.tick_size);
        payload["tpTriggerBy"] = "LastPrice";
    }

    const int ret = SendRestRequest("POST", "/v5/order/create", payload.dump());
    (void)fill_price;
    (void)current_time;

    if (ret == 0) {
        // accepted but not filled yet. reporting it as open closes the window
        // in which SinglePositionLock would admit a second entry before the
        // exchange echo arrives.
        std::lock_guard<std::mutex> lock(state_mutex_);
        pending_entry_ = true;
        pending_since_ms_ = NowMillis();
    } else {
        LOG_ERROR("BybitBroker: entry rejected by venue (retCode {})", ret);
    }
}

void BybitBroker::CheckRisk(double current_price, int64_t current_time) {
    // intentionally empty. the hard stop and the target are attached to the
    // entry, so bybit's matching engine enforces them even if this process
    // dies. break-even, trailing and scale-out are PositionManager's job: it
    // measures every threshold against the placed stop and calls
    // UpdateStopLoss / ClosePartial.
    //
    // running a second ladder here would move the same stop twice, in two
    // different units -- which is exactly the paper/live divergence that made
    // venue results unattributable against backtest.
    (void)current_price;
    (void)current_time;
}

void BybitBroker::UpdateStopLoss(double new_sl) {
    if (!(new_sl > 0.0)) {
        LOG_ERROR("BybitBroker: refusing to push a non-positive stop ({})",
                  new_sl);
        return;
    }

    // round before sending. bybit rejects an off-tick price with the whole
    // request (retCode 10001), and on a break-even or trail move that means
    // the position keeps its previous stop while the ladder moves on.
    const double rounded_sl = RoundToStep(new_sl, rules_.tick_size);

    nlohmann::json payload = {
        {"category", spec_.category},
        {"symbol", spec_.symbol},
        {"positionIdx", 0},
        {"stopLoss", FormatStep(rounded_sl, rules_.tick_size)},
        {"slTriggerBy", "LastPrice"},
    };

    // commit only on acceptance. Writing locally first left the engine
    // believing a rejected move had landed; PositionManager's never-move-
    // backwards comparison then skipped every retry, and the position sat
    // on the old stop with a log line claiming it had moved.
    const int ret =
        SendRestRequest("POST", "/v5/position/trading-stop", payload.dump());
    if (ret == 0) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_sl_ = rounded_sl;
        current_position_.stop_loss = rounded_sl;
    } else {
        LOG_ERROR("BybitBroker: stop-loss move to {} rejected (retCode {})",
                  rounded_sl, ret);
    }
}

void BybitBroker::ClosePartial(double fraction, double current_price,
                               int64_t current_time, const std::string& reason) {
    (void)current_price;
    (void)current_time;

    double close_size = 0.0;
    bool closing_long = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (position_size_ <= 0.0) return;

        const double clamped = std::max(0.0, std::min(1.0, fraction));
        close_size = position_size_ * clamped;
        if (!(close_size > 0.0)) return;

        closing_long = current_position_.direction == SignalDirection::BUY;

        // reduce optimistically: LiveOrchestrator::Stop calls this to flatten,
        // and a repeated call must never send a second order for a position
        // that is already gone. leaving an open position behind on shutdown is
        // the one failure a prop account does not forgive.
        position_size_ -= close_size;
        if (position_size_ < 0.0) position_size_ = 0.0;
        current_position_.volume = position_size_;
        if (position_size_ == 0.0) {
            current_position_.direction = SignalDirection::NONE;
        }
        last_closed_size_ = close_size;
    }

    nlohmann::json payload = {
        {"category", spec_.category},
        {"symbol", spec_.symbol},
        {"side", closing_long ? "Sell" : "Buy"},
        {"orderType", "Market"},
        {"qty", FormatStep(close_size, rules_.qty_step)},
        {"reduceOnly", true},
        {"orderLinkId", "close-" + std::to_string(NowSignedMillis()) + "-" +
                            std::to_string(++order_seq_)},
    };

    LOG_INFO("BybitBroker: reducing position by {} ({})", close_size, reason);
    SendRestRequest("POST", "/v5/order/create", payload.dump());
}
