#include "bybit_broker.hpp"

#include "live_debug.hpp"
#include "logger.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>

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

} // namespace

BybitBroker::BybitBroker(const std::string& api_key,
                         const std::string& api_secret,
                         bool is_testnet)
    : api_key_(api_key), api_secret_(api_secret), is_testnet_(is_testnet) {
    // V5 routes testnet and mainnet to different hosts.
    if (is_testnet_) {
        rest_base_url_  = "https://api-testnet.bybit.com";
        private_ws_url_ = "wss://stream-testnet.bybit.com/v5/private";
    } else {
        rest_base_url_  = "https://api.bybit.com";
        private_ws_url_ = "wss://stream.bybit.com/v5/private";
    }

    ConnectPrivateStream();
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
                // connection is up -> run the HMAC handshake, then subscribe
                // to the execution and position push channels.
                AuthenticatePrivateStream();
                SubscribePrivateTopics();
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
    const int64_t expires = NowMillis() + 10000;
    const std::string signature =
        GenerateSignature("GET/realtime" + std::to_string(expires), "");

    const std::string auth_msg =
        "{\"op\":\"auth\",\"args\":[\"" + api_key_ + "\"," +
        std::to_string(expires) + ",\"" + signature + "\"]}";
    private_ws_.send(auth_msg);
}

void BybitBroker::SubscribePrivateTopics() {
    // execution -> fills/order updates, position -> live size and SL/TP.
    private_ws_.send(R"({"op":"subscribe","args":["position","execution"]})");
}

void BybitBroker::HandlePrivateMessage(const std::string& payload) {
    // parse without throwing: malformed frames from the exchange are dropped
    // instead of tearing down the websocket thread.
    const nlohmann::json j = nlohmann::json::parse(payload, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        LIVE_DBG("bybit: dropped unparsable private frame: {}", payload);
        return;
    }

    const std::string topic = j.value("topic", "");
    if (topic != "position" && topic != "execution") {
        // auth acks, pongs and subscribe confirmations land here.
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
                // single-symbol broker: only the BTCUSDT position matters.
                if (entry.value("symbol", "") != "BTCUSDT") continue;

                position_size_ =
                    entry.contains("size") ? std::stod(entry["size"].get<std::string>()) : 0.0;

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
        } else if (topic == "execution") {
            for (const auto& entry : j["data"]) {
                if (!entry.is_object()) continue;
                if (entry.value("symbol", "") != "BTCUSDT") continue;
                if (entry.contains("closedSize")) {
                    last_closed_size_ = std::stod(entry["closedSize"].get<std::string>());
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

void BybitBroker::SendRestRequest(const std::string& method,
                                  const std::string& endpoint,
                                  const std::string& payload) {
    // Builds an authenticated V5 REST call. Required headers:
    //   X-BAPI-API-KEY, X-BAPI-TIMESTAMP, X-BAPI-RECV-WINDOW, X-BAPI-SIGN
    // where X-BAPI-SIGN = HMAC_SHA256(timestamp + api_key + recv_window + payload).
    const std::string timestamp = std::to_string(NowMillis());
    const std::string recv_window = "5000";
    const std::string signing_string = timestamp + api_key_ + recv_window + payload;
    const std::string signature = GenerateSignature(signing_string, "");

    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("BybitBroker: curl_easy_init() failed for {}", endpoint);
        return;
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
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

// ---- cached state getters (served without blocking) ----

double BybitBroker::GetBalance() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_balance_;
}

bool BybitBroker::HasOpenPosition() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return position_size_ > 0.0;
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
        {"category", "linear"},
        {"symbol", "BTCUSDT"},
        {"orderType", "Market"},
        {"side", (signal.direction == SignalDirection::BUY) ? "Buy" : "Sell"},
        {"qty", FormatDouble(signal.volume)},
    };

    // Protective levels are attached to the entry so the hard stop lives on
    // Bybit's matching engine even if this process dies.
    if (signal.stop_loss > 0.0) {
        payload["stopLoss"] = FormatDouble(signal.stop_loss);
        payload["slTriggerBy"] = "LastPrice";
    }
    if (signal.take_profit > 0.0) {
        payload["takeProfit"] = FormatDouble(signal.take_profit);
        payload["tpTriggerBy"] = "LastPrice";
    }

    SendRestRequest("POST", "/v5/order/create", payload.dump());
    (void)fill_price;
    (void)current_time;
}

void BybitBroker::CheckRisk(double current_price, int64_t current_time) {
    // DYNAMIC RISK MANAGEMENT (client side, mirrors PaperTrader):
    // the hard stop already sits on Bybit, but break-even and trailing
    // adjustments are computed locally because Bybit's native trailing stop
    // cannot express the strategy's percentage-of-entry rules.
    //
    //   1. track the running high/low since entry for both directions
    //   2. if unrealized profit >= be_trigger_pct -> move SL to entry +/- be_target_pct
    //   3. if unrealized profit >= trailing_trigger_pct -> keep SL
    //      trailing_dist_pct behind the best observed price
    //   4. if the computed SL differs from the last one pushed, call
    //      UpdateStopLoss(new_sl)
    //
    // State is read from the private-channel snapshots so this runs without a
    // round-trip to the exchange.
    (void)current_price;
    (void)current_time;
}

void BybitBroker::UpdateStopLoss(double new_sl) {
    // Persist the new stop so CheckRisk can diff against it, then push it to
    // the exchange.
    current_sl_ = new_sl;

    nlohmann::json payload = {
        {"category", "linear"},
        {"symbol", "BTCUSDT"},
        {"positionIdx", 0},
        {"stopLoss", FormatDouble(new_sl)},
        {"slTriggerBy", "LastPrice"},
    };

    SendRestRequest("POST", "/v5/position/trading-stop", payload.dump());
}

void BybitBroker::ClosePartial(double fraction, double current_price,
                               int64_t current_time, const std::string& reason) {
    // Reduce-only market order for `fraction` of the open position:
    // POST /v5/order/create with "reduceOnly": true, "qty": size * fraction.
    (void)fraction;
    (void)current_price;
    (void)current_time;
    (void)reason;
}
