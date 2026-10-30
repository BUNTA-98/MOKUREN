#include "blofin_broker.hpp"

#include "blofin_client_order_id.hpp"
#include "live_debug.hpp"
#include "logger.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr int kHeartbeatIntervalMs = 25000;
constexpr int kHeartbeatSliceMs = 100;
constexpr int64_t kPendingEntryTimeoutMs = 10000;

double JsonNum(const nlohmann::json& j, const char* key, double fallback = 0.0) {
    if (!j.contains(key)) return fallback;
    const auto& v = j[key];
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        try { return std::stod(v.get<std::string>()); } catch (...) { return fallback; }
    }
    return fallback;
}

// the account-level equity of a balance reply, or nullopt when the frame
// carries no data object at all. accepts the object shape and the
// single-element array shape, and both spelling sets seen on this venue:
// totalEquity/equity are the documented ones, totalEq/eq are okx-lineage.
//
// split out of ReconcileAccount so the COIN-FUTURES diagnostic below reports
// a number by the same rules the USDT-FUTURES path just used -- two readers
// that disagree about where equity lives would make the diagnostic lie.
std::optional<double> BalanceEquity(const nlohmann::json& bal) {
    const nlohmann::json* data = nullptr;
    if (bal.is_object() && bal.contains("data")) {
        const nlohmann::json& d = bal["data"];
        if (d.is_object()) {
            data = &d;
        } else if (d.is_array() && !d.empty() && d[0].is_object()) {
            data = &d[0];
        }
    }
    if (data == nullptr) return std::nullopt;

    double equity = JsonNum(*data, "totalEquity");
    if (!(equity > 0.0)) equity = JsonNum(*data, "totalEq");
    if (!(equity > 0.0) && data->contains("details") &&
        (*data)["details"].is_array()) {
        for (const auto& row : (*data)["details"]) {
            if (!row.is_object()) continue;
            double e = JsonNum(row, "equity");
            if (!(e > 0.0)) e = JsonNum(row, "eq");
            if (e > 0.0) { equity = e; break; }
        }
    }
    return equity;
}

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

std::string Base64Of(const std::string& ascii) {
    if (ascii.empty()) return std::string();
    const size_t out_len = 4 * ((ascii.size() + 2) / 3);
    std::string out(out_len, '\0');
    const int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(&out[0]),
        reinterpret_cast<const unsigned char*>(ascii.data()),
        static_cast<int>(ascii.size()));
    if (written < 0) return std::string();
    out.resize(static_cast<size_t>(written));
    return out;
}

int64_t NowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t total = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
    return total;
}

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

double RoundToStep(double value, double step) {
    if (!(step > 0.0)) return value;
    return std::round(value / step) * step;
}

// OKX-lineage reply: top-level `code == "0"` only means "accepted for
// processing". The per-order/-tpsl/-algo result lives in `data.code` (single
// object) or `data[0].code` (array). Return 0 = success, non-0 = rejected,
// -1 = body unparsable / no code available.
int AckCode(const nlohmann::json& body) {
    if (!body.is_object()) return -1;
    auto as_code = [](const nlohmann::json& v) -> int {
        if (v.is_string()) return v.get<std::string>() == "0" ? 0 : 1;
        if (v.is_number_integer()) return v.get<int64_t>() == 0 ? 0 : 1;
        return -1;
    };
    const auto data = body.value("data", nlohmann::json{});
    if (data.is_object() && data.contains("code"))
        return as_code(data["code"]);
    if (data.is_array() && !data.empty() && data[0].is_object() &&
        data[0].contains("code"))
        return as_code(data[0]["code"]);
    return 0;  // no per-item code: trust top-level (plain GET-style replies)
}

std::string AckMessage(const nlohmann::json& body) {
    if (!body.is_object()) return "unparsable reply";
    const auto data = body.value("data", nlohmann::json{});
    auto inner = [](const nlohmann::json& d) -> std::string {
        if (d.is_object()) return d.value("msg", std::string{});
        if (d.is_array() && !d.empty() && d[0].is_object())
            return d[0].value("msg", std::string{});
        return {};
    };
    const std::string m = inner(data);
    if (!m.empty()) return m;
    const std::string top = body.value("msg", std::string{});
    return top.empty() ? "no message" : top;
}

// "BTCUSDT" -> "BTC-USDT". Idempotent if already hyphenated.
std::string WireSymbolFor(const std::string& symbol) {
    if (symbol.find('-') != std::string::npos) return symbol;
    for (const char* quote : {"USDT", "USDC", "USD"}) {
        const std::string q(quote);
        if (symbol.size() > q.size() &&
            symbol.compare(symbol.size() - q.size(), q.size(), q) == 0) {
            return symbol.substr(0, symbol.size() - q.size()) + "-" + q;
        }
    }
    return symbol;
}

} // namespace

std::atomic<uint64_t> BloFinBroker::nonce_counter_{0};

std::string BloFinBroker::MakeNonce() {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    const uint64_t counter = nonce_counter_.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream oss;
    oss << std::hex << ns << "-" << counter;
    return oss.str();
}

int64_t BloFinBroker::NowSignedMillis() const {
    return NowMillis() + clock_offset_ms_;
}

BloFinBroker::BloFinBroker(const std::string& api_key,
                           const std::string& api_secret,
                           const std::string& api_passphrase,
                           mokuren::venue::Spec spec,
                           std::string public_rest_base)
    : api_key_(api_key), api_secret_(api_secret),
      api_passphrase_(api_passphrase), spec_(spec) {
    // demo-only guard. remove when mainnet is proven end-to-end.
    if (spec_.env != mokuren::venue::Env::Demo) {
        throw std::runtime_error(
            "BloFinBroker: execution is demo-only in this build (spec env is '" +
            std::string(mokuren::venue::EnvName(spec_.env)) +
            "'); remove the guard in blofin_broker.cpp when mainnet is proven");
    }

    rest_base_url_    = spec_.rest_base;
    market_rest_base_ = public_rest_base;
    private_ws_url_   = spec_.private_ws;
    wire_symbol_      = WireSymbolFor(spec_.symbol);

    LOG_INFO("BloFinBroker: venue {}", mokuren::venue::Manifest(spec_));
    // print the exact private-ws URL at INFO, not just under --debug. the
    // 403 investigation turned on "which host is /ws/private on", and the
    // answer has to be in the log of the run that failed, not in a doc
    // lookup done afterwards. this line is the one to grep for.
    LOG_INFO("BloFinBroker: private ws url {}", private_ws_url_);
    if (wire_symbol_ != spec_.symbol) {
        LOG_INFO("BloFinBroker: wire symbol {} (config symbol {})",
                 wire_symbol_, spec_.symbol);
    }

    EstablishStartupTruth();
    ConnectPrivateStream();
}

BloFinBroker::BloFinBroker(const std::string& api_key,
                           const std::string& api_secret,
                           const std::string& api_passphrase,
                           mokuren::venue::Spec spec,
                           OfflineForTests)
    : api_key_(api_key), api_secret_(api_secret),
      api_passphrase_(api_passphrase), spec_(spec) {
    rest_base_url_  = spec_.rest_base;
    private_ws_url_ = spec_.private_ws;
    wire_symbol_    = WireSymbolFor(spec_.symbol);
}

void BloFinBroker::EstablishStartupTruth() {
    SyncClock();
    if (!FetchInstrumentRules()) {
        throw std::runtime_error(
            "BloFinBroker: could not read instrument rules for " + wire_symbol_ +
            " -- refusing to trade blind");
    }
    ReconcileAccount();
    VerifyAccountMode();
}

void BloFinBroker::SyncClock() {
    // no /market/time on blofin: read `ts` from the ticker.
    nlohmann::json j;
    if (!SendMarketGet("/api/v1/market/tickers",
                       "instId=" + wire_symbol_, j)) {
        throw std::runtime_error(
            "BloFinBroker: cannot read ticker for clock sync from " +
            rest_base_url_);
    }
    if (!j.contains("data") || !j["data"].is_array() || j["data"].empty()) {
        throw std::runtime_error("BloFinBroker: ticker reply carried no data");
    }
    int64_t server_ms = 0;
    try {
        const auto& row = j["data"][0];
        if (row.contains("ts")) {
            server_ms = std::stoll(row["ts"].is_string()
                                       ? row["ts"].get<std::string>()
                                       : std::to_string(row["ts"].get<int64_t>()));
        }
    } catch (...) {}
    if (server_ms <= 0) {
        throw std::runtime_error("BloFinBroker: ticker reply carried no ts");
    }
    // this is an ESTIMATE, not the answer: the ticker's `ts` is the timestamp
    // of the data it describes, so a cached or quiet feed reports something
    // seconds old. measured -10 s and -14 s here on a host whose NTP was in
    // sync, and the balance reply's own `ts` agreed with the local clock to the
    // second -- i.e. the local clock was never the problem. RefineClockFromReply
    // replaces this once a signed reply arrives. keep it anyway: it is what lets
    // the FIRST signed request pass when the local clock really is hours off.
    clock_offset_ms_ = server_ms - NowMillis();
    LOG_INFO("BloFinBroker: clock offset estimate {} ms (from ticker data ts)",
             clock_offset_ms_);
    if (clock_offset_ms_ > 1000 || clock_offset_ms_ < -1000) {
        LOG_WARN("BloFinBroker: ticker data is {} ms behind the local clock; "
                 "refining from a signed reply before trusting that as drift",
                 clock_offset_ms_ > 0 ? clock_offset_ms_ : -clock_offset_ms_);
    }
}

void BloFinBroker::RefineClockFromReply(const nlohmann::json& reply,
                                        const char* source) {
    int64_t server_ms = 0;
    try {
        if (reply.is_object() && reply.contains("data") &&
            reply["data"].is_object() && reply["data"].contains("ts")) {
            const auto& v = reply["data"]["ts"];
            server_ms = std::stoll(v.is_string() ? v.get<std::string>()
                                                 : std::to_string(v.get<int64_t>()));
        }
    } catch (...) {}
    // no `ts` on this reply: nothing to refine with, keep the estimate.
    if (server_ms <= 0) return;

    const int64_t measured = server_ms - NowMillis();
    const int64_t delta = measured - clock_offset_ms_;
    const int64_t abs_delta = delta < 0 ? -delta : delta;
    if (abs_delta >= 1000) {
        LOG_WARN("BloFinBroker: {} puts the venue {} ms away, not {}; the "
                 "ticker ts was stale data, not clock drift -- using {}",
                 source, measured, clock_offset_ms_, measured);
    }
    clock_offset_ms_ = measured;
}

bool BloFinBroker::FetchInstrumentRules() {
    nlohmann::json j;
    if (!SendMarketGet("/api/v1/market/instruments",
                       "instId=" + wire_symbol_, j)) return false;
    if (!j.contains("data") || !j["data"].is_array() || j["data"].empty()) {
        return false;
    }
    // Pick the row whose instId actually matches. A filter that isn't honoured
    // (unlikely, but cheap to defend) must not silently borrow another
    // symbol's tickSize / lotSize.
    const nlohmann::json* match = nullptr;
    for (const auto& row : j["data"]) {
        if (row.is_object() && row.value("instId", std::string{}) == wire_symbol_) {
            match = &row;
            break;
        }
    }
    if (!match) {
        LOG_ERROR("BloFinBroker: instruments reply has no row for {}",
                  wire_symbol_);
        return false;
    }
    const nlohmann::json& item = *match;

    const std::string state = item.value("state", std::string{});
    if (state != "live") {
        LOG_ERROR("BloFinBroker: {} state is '{}' (not live); refusing to trade",
                  wire_symbol_, state);
        return false;
    }
    if (item.value("apiPermission", std::string("full")) == "reduce_only") {
        LOG_ERROR("BloFinBroker: {} is reduce_only via API; entries will be "
                  "rejected by the venue. Refusing to trade.", wire_symbol_);
        return false;
    }

    InstrumentRules r;
    r.tick_size      = JsonNum(item, "tickSize");
    r.qty_step       = JsonNum(item, "lotSize");
    r.min_qty        = JsonNum(item, "minSize");
    r.min_notional   = 0.0;
    r.max_leverage   = JsonNum(item, "maxLeverage");
    r.contract_value = JsonNum(item, "contractValue");
    if (!(r.qty_step > 0.0) || !(r.contract_value > 0.0)) {
        LOG_ERROR("BloFinBroker: instruments reply missing lotSize/contractValue");
        return false;
    }
    r.valid = true;
    rules_ = r;
    contract_value_ = r.contract_value;
    LOG_INFO("BloFinBroker: {} tick={} lot={} minSize={} contract={} maxLev={}",
             wire_symbol_, r.tick_size, r.qty_step, r.min_qty,
             r.contract_value, r.max_leverage);
    return true;
}

void BloFinBroker::ReconcileAccount() {
    // Prime balance from REST before the private ws `account` push lands.
    // Without this, GetBalance()/GetEquity() return 0 until the first ws
    // frame arrives, and any sizing/risk path that divides by equity or
    // treats 0 as "no margin" silently refuses to trade -- which reads as
    // "the venue rejected us" when really we never had a number.
    //
    // Hard-fail like FetchInstrumentRules: trading blind on equity is worse
    // than refusing to start.
    {
        // the "0 or unreadable" verdict cannot tell an unfunded demo account
        // from a renamed field, and the two need opposite fixes: one is funded
        // on the venue, the other is a parser change here. carry the raw reply
        // into the failure so the run itself answers that question.
        nlohmann::json bal;
        if (!SendRestGet("/api/v1/account/balance",
                         "productType=USDT-FUTURES", bal)) {
            throw std::runtime_error(
                "BloFinBroker: could not read account balance at startup");
        }
        LIVE_DBG("blofin: balance reply {}", bal.dump());
        // refine BEFORE the equity check: a zero-balance reply is still a valid
        // server timestamp, and it is the one that corrects the estimate even
        // on the run that aborts.
        RefineClockFromReply(bal, "balance reply");
        // some builds wrap `data` in an array even for the account-level
        // reply. a strict is_object() turned a valid frame into "carried no
        // data object".
        const std::optional<double> parsed = BalanceEquity(bal);
        if (!parsed.has_value()) {
            throw std::runtime_error(
                "BloFinBroker: balance reply carried no data object (raw "
                "reply: " + bal.dump() + ")");
        }
        const double equity = *parsed;
        if (!(equity > 0.0)) {
            // a successful `code: "0"` reply with all-zero numbers is a
            // different situation from a failed GET, and the two are one
            // doc-lookup apart. say which one this is: the signature and the
            // credentials are proven by the fact that we got here at all.
            //
            // the query above is scoped to USDT-FUTURES. virtual demo funds
            // frequently sit in the coin-margin wallet instead, and an
            // all-zero USDT-FUTURES reply is indistinguishable from an account
            // holding nothing. one extra call on a path that is already
            // aborting decides which of the two this is, instead of sending
            // the operator to fund a wallet that is not the problem.
            std::string coin_line = "the COIN-FUTURES wallet could not be read";
            {
                nlohmann::json alt;
                if (SendRestGet("/api/v1/account/balance",
                                "productType=COIN-FUTURES", alt)) {
                    const std::optional<double> alt_eq = BalanceEquity(alt);
                    if (alt_eq.has_value() && *alt_eq > 0.0) {
                        coin_line =
                            "the COIN-FUTURES wallet holds " +
                            std::to_string(*alt_eq) +
                            ", so the funds are in a product line this config "
                            "does not trade";
                    } else {
                        coin_line = "the COIN-FUTURES wallet is empty too";
                    }
                }
            }
            throw std::runtime_error(
                "BloFinBroker: account balance is 0 or unreadable at startup "
                "-- refusing to trade without margin. the signed GET succeeded "
                "with code 0, so the key, secret, passphrase and host are "
                "valid; this is an unfunded account, not a credential problem. "
                "fund it on the venue. (" + coin_line +
                ") (raw reply: " + bal.dump() + ")");
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_balance_ = equity;
        LOG_INFO("BloFinBroker: startup equity {}", equity);
    }

    nlohmann::json j;
    if (!SendRestGet("/api/v1/account/positions",
                     "instId=" + wire_symbol_, j)) {
        LOG_ERROR("BloFinBroker: could not read open positions at startup");
        return;
    }
    if (!j.contains("data") || !j["data"].is_array()) return;
    for (const auto& p : j["data"]) {
        if (!p.is_object()) continue;
        if (p.value("instId", "") != wire_symbol_) continue;
        const double sz = p.contains("positions") ? JsonNum(p, "positions")
                                                  : JsonNum(p, "pos");
        if (sz == 0.0) continue;
        std::lock_guard<std::mutex> lock(state_mutex_);
        position_size_ = std::abs(sz);
        position_dir_  = (sz > 0.0) ? SignalDirection::BUY : SignalDirection::SELL;
        entry_price_   = p.contains("averagePrice") ? JsonNum(p, "averagePrice")
                                                    : JsonNum(p, "avgPrice");
        current_sl_    = 0.0;
        current_tp_    = 0.0;
        current_position_.direction   = position_dir_;
        current_position_.volume      = position_size_;
        current_position_.entry_price = entry_price_;
        // informational, not a defect. the TPSL recovery below is what looks
        // for a live stop, so claiming "no SL/TP cached" here fired before that
        // lookup had a chance to find one.
        LOG_INFO("BloFinBroker: adopted existing {} {} size={} entry={}",
                 (position_dir_ == SignalDirection::BUY ? "long" : "short"),
                 wire_symbol_, position_size_, entry_price_);
    }

    // Recover the live TPSL id and its SL so PM's ladder has a 1R to measure.
    // Without this, GetCurrentPosition().stop_loss == 0, PositionManager's
    // `risk_distance > 0` guard bails on every tick, and a restart silently
    // downgrades BE/trail to venue-only.
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (position_size_ <= 0.0) return;
    }
    nlohmann::json tpsl;
    if (SendRestGet("/api/v1/trade/orders-tpsl-pending",
                    "instId=" + wire_symbol_, tpsl) &&
        tpsl.contains("data") && tpsl["data"].is_array()) {
        for (const auto& row : tpsl["data"]) {
            if (!row.is_object()) continue;
            if (row.value("instId", "") != wire_symbol_) continue;
            if (row.value("state", "") != "live") continue;
            const std::string id = row.value("tpslId", "");
            if (id.empty()) continue;
            const double sl = JsonNum(row, "slTriggerPrice");
            const double tp = JsonNum(row, "tpTriggerPrice");
            std::lock_guard<std::mutex> lock(state_mutex_);
            current_tpsl_id_ = id;
            if (sl > 0.0) { current_sl_ = sl;
                            current_position_.stop_loss = sl; }
            if (tp > 0.0) { current_tp_ = tp;
                            current_position_.take_profit = tp; }
            LOG_INFO("BloFinBroker: recovered TPSL {} sl={} tp={}",
                     id, sl, tp);
            break;
        }
    }

    // an adopted position with no stop is the one state this adapter will not
    // repair for you: it has no 1R to measure, so PositionManager's ladder bails
    // on every tick and the position sits with venue-only protection -- none.
    // say it as an error at startup, where it can still change what you do.
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (position_size_ > 0.0 && !(current_sl_ > 0.0)) {
            LOG_ERROR("BloFinBroker: adopted a {} position size={} with NO "
                      "stop; trade management cannot arm without a 1R and the "
                      "position is unprotected. close it on the venue (or place "
                      "a stop there) before treating this run as a test",
                      position_dir_ == SignalDirection::BUY ? "long" : "short",
                      position_size_);
        }
    }
}

void BloFinBroker::VerifyAccountMode() {
    // Orders below are sent with positionSide="net" and marginMode="cross".
    // Both are only meaningful in net_mode / cross; a wrong mode turns
    // reduceOnly into "ignored" (per the docs) and silently breaks closes.
    // Refuse to start rather than trade in a mode we don't understand.
    nlohmann::json pm;
    if (!SendRestGet("/api/v1/account/position-mode", "", pm)) {
        throw std::runtime_error(
            "BloFinBroker: could not read position-mode at startup");
    }
    const std::string mode = (pm.contains("data") && pm["data"].is_object())
                                 ? pm["data"].value("positionMode", "")
                                 : "";
    if (mode != "net_mode") {
        throw std::runtime_error(
            "BloFinBroker: position-mode is '" + mode +
            "'; this broker requires net_mode (one-way). Change it with "
            "POST /api/v1/account/set-position-mode first.");
    }

    nlohmann::json mm;
    if (!SendRestGet("/api/v1/account/margin-mode", "", mm)) {
        throw std::runtime_error(
            "BloFinBroker: could not read margin-mode at startup");
    }
    const std::string margin = (mm.contains("data") && mm["data"].is_object())
                                   ? mm["data"].value("marginMode", "")
                                   : "";
    if (margin != "cross") {
        throw std::runtime_error(
            "BloFinBroker: margin-mode is '" + margin +
            "'; this broker requires cross margin.");
    }
    LOG_INFO("BloFinBroker: account is net_mode/cross -- safe to trade");
}

BloFinBroker::~BloFinBroker() { Stop(); }

void BloFinBroker::Stop() {
    if (!keep_alive_.exchange(false, std::memory_order_acq_rel)) return;
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    private_ws_.stop();
    ws_connected_ = false;
    ws_authed_.store(false, std::memory_order_release);
}

void BloFinBroker::ConnectPrivateStream() {
    private_ws_.setUrl(private_ws_url_);
    private_ws_.enableAutomaticReconnection();
    private_ws_.setPingInterval(20);

    // ix::WebSocket's default handshake sends only the RFC 6455 minimum
    // (Connection/Upgrade/Sec-WebSocket-Version/Key/Host). BloFin's edge
    // refuses that shape on /ws/private with a 403 while the REST host on the
    // same name answers normally -- the upgrade is being filtered on the
    // absence of a browser-shaped Origin/User-Agent, not on the credentials
    // (which are not in the handshake at all; they arrive in the login frame
    // after the socket is open). Set both so the upgrade looks like the one
    // the venue's own web client sends.
    //
    // Origin must match the host being dialed, not a fixed string: the demo
    // and mainnet hosts are different names, and an Origin naming the other
    // world is a fresh reason to reject. Derive it from private_ws_url_ so a
    // host-table change in venue_spec.cpp cannot leave a stale Origin here.
    std::string origin = private_ws_url_;
    if (origin.compare(0, 6, "wss://") == 0) {
        origin = "https://" + origin.substr(6);
    } else if (origin.compare(0, 5, "ws://") == 0) {
        origin = "http://" + origin.substr(5);
    }
    // strip the path: Origin is scheme://host[:port], never a path.
    const std::string::size_type slash = origin.find('/', origin.find("://") + 3);
    if (slash != std::string::npos) origin.resize(slash);

    private_ws_.setExtraHeaders({
        {"Origin", origin},
        {"User-Agent",
         "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
         "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"},
    });
    LIVE_DBG("blofin: dialing private ws {} (Origin: {})",
             private_ws_url_, origin);

    private_ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
            case ix::WebSocketMessageType::Open:
                // a (re)connect invalidates any previous login: the socket is
                // up but fills cannot arrive until the login frame is acked.
                ws_authed_.store(false, std::memory_order_release);
                // reset the retry-log latch so a reconnect that succeeds and
                // later fails again logs a fresh ERROR, not a suppressed WARN.
                last_logged_retries_.store(-1, std::memory_order_relaxed);
                LOG_INFO("BloFinBroker: private stream connected -- "
                         "authenticating");
                AuthenticatePrivateStream();
                ws_connected_ = true;
                break;
            case ix::WebSocketMessageType::Message:
                HandlePrivateMessage(msg->str);
                break;
            case ix::WebSocketMessageType::Error: {
                // one line per DISTINCT cause, not per retry. a WAF refusing
                // the upgrade returns an identical 403 every ~2 s forever;
                // logging all of them buries the first occurrence, which is
                // the only informative one.
                //
                // gated on keep_alive_ for the same reason as Close below: an
                // Error frame can arrive from ix's receive thread after Stop()
                // has already torn the socket down, and "still failing after N
                // retries" on a run that is exiting is a lie that costs a grep
                // to disprove.
                if (keep_alive_.load(std::memory_order_acquire)) {
                    const int r = msg->errorInfo.retries;
                    const int last =
                        last_logged_retries_.load(std::memory_order_relaxed);
                    if (r == 0 || r != last) {
                        last_logged_retries_.store(r, std::memory_order_relaxed);
                        if (r == 0) {
                            LOG_ERROR("BloFinBroker: private stream error "
                                      "(http={}): {}",
                                      msg->errorInfo.http_status,
                                      msg->errorInfo.reason);
                        } else {
                            LOG_WARN("BloFinBroker: private stream error "
                                     "(http={}) still failing after {} retries: "
                                     "{}",
                                     msg->errorInfo.http_status, r,
                                     msg->errorInfo.reason);
                        }
                    }
                } else {
                    LIVE_DBG("BloFinBroker: private stream error on shutdown "
                             "(http={}): {}",
                             msg->errorInfo.http_status,
                             msg->errorInfo.reason);
                }
                ws_connected_ = false;
                ws_authed_.store(false, std::memory_order_release);
                break;
            }
            case ix::WebSocketMessageType::Close:
                // ix delivers Close from its own receive thread even when the
                // close was requested by Stop(). "reconnecting" on a run that
                // is exiting is misleading: keep_alive_ is false and the
                // socket will not try again. say which case this is.
                if (keep_alive_.load(std::memory_order_acquire)) {
                    LOG_WARN("BloFinBroker: private stream closed (code={} "
                             "reason='{}'); reconnecting",
                             msg->closeInfo.code, msg->closeInfo.reason);
                } else {
                    LIVE_DBG("BloFinBroker: private stream closed on shutdown "
                             "(code={})", msg->closeInfo.code);
                }
                ws_connected_ = false;
                ws_authed_.store(false, std::memory_order_release);
                break;
            default:
                break;
        }
    });

    keep_alive_.store(true, std::memory_order_release);
    heartbeat_thread_ = std::thread(&BloFinBroker::HeartbeatLoop, this);
    private_ws_.start();
}

void BloFinBroker::HeartbeatLoop() {
    while (keep_alive_.load(std::memory_order_acquire)) {
        for (int waited = 0; waited < kHeartbeatIntervalMs;
             waited += kHeartbeatSliceMs) {
            if (!keep_alive_.load(std::memory_order_acquire)) return;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kHeartbeatSliceMs));
        }
        if (!keep_alive_.load(std::memory_order_acquire)) return;
        if (ws_connected_.load(std::memory_order_acquire)) {
            // blofin ping is the raw string "ping", not JSON.
            LIVE_DBG("blofin: sending heartbeat ping");
            private_ws_.send("ping");
        }
    }
}

void BloFinBroker::AuthenticatePrivateStream() {
    const std::string ts_ms = std::to_string(NowSignedMillis());
    const std::string nonce = ts_ms;   // docs: nonce MAY equal timestamp
    const std::string sign = GenerateSignature("/users/self/verify", "GET",
                                               ts_ms, nonce, "");
    LOG_INFO("BloFinBroker: sending ws login (ts={})", ts_ms);
    nlohmann::json login;
    login["op"] = "login";
    login["args"] = nlohmann::json::array();
    login["args"].push_back({
        {"apiKey",     api_key_},
        {"passphrase", api_passphrase_},
        {"timestamp",  ts_ms},
        {"sign",       sign},
        {"nonce",      nonce},
    });
    private_ws_.send(login.dump());
}

void BloFinBroker::SubscribePrivateTopics() {
    nlohmann::json sub;
    sub["op"] = "subscribe";
    sub["args"] = nlohmann::json::array();
    sub["args"].push_back({{"channel", "positions"},   {"instId", wire_symbol_}});
    sub["args"].push_back({{"channel", "orders"},      {"instId", wire_symbol_}});
    sub["args"].push_back({{"channel", "account"}});
    private_ws_.send(sub.dump());
}

void BloFinBroker::HandlePrivateMessage(const std::string& payload) {
    if (payload == "pong") { LIVE_DBG("blofin: pong"); return; }
    // raw frame under --debug only: the subscribe-ack shape is inferred from
    // OKX-lineage convention, and if the venue answers with a different
    // envelope (e.g. {"op":"subscribe","success":true,...}) the branch below
    // will not match and the ack falls through silently again. one line here
    // turns the next iteration into a literal fix instead of a guess.
    LIVE_DBG("blofin: private raw frame {}", payload.substr(0, 200));
    const nlohmann::json j = nlohmann::json::parse(payload, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;

    if (j.contains("event")) {
        const std::string event = j.value("event", "");
        if (event == "login") {
            const std::string code = j.value("code", std::string("-1"));
            if (code == "0") {
                LOG_INFO("BloFinBroker: private stream authenticated -- "
                         "subscribing");
                ws_authed_.store(true, std::memory_order_release);
                SubscribePrivateTopics();
            } else {
                LOG_ERROR("BloFinBroker: login failed (code {}): {}",
                          code, j.value("msg", "no reason"));
                ws_authed_.store(false, std::memory_order_release);
            }
        } else if (event == "subscribe") {
            // the ack is the direct proof that the venue accepted the
            // subscription for that channel + instId. `account` is
            // account-level and carries no instId, so it can flow while
            // `positions`/`orders` (subscribed per-instrument) are silently
            // rejected on a wrong wire symbol. logging the channel name is
            // what makes that visible: three acks = all accepted, a missing
            // one names the channel that was not.
            const std::string channel =
                j.contains("arg") && j["arg"].is_object()
                    ? j["arg"].value("channel", std::string{})
                    : std::string{};
            LOG_INFO("BloFinBroker: private stream subscribe ack channel='{}'",
                     channel);
        } else if (event == "error") {
            // name the channel too: a failed subscribe is otherwise
            // indistinguishable from a session-level error, and the fix
            // (usually the instId format) is per-channel.
            const std::string channel =
                j.contains("arg") && j["arg"].is_object()
                    ? j["arg"].value("channel", std::string{})
                    : std::string{};
            LOG_ERROR("BloFinBroker: private stream error (code {}) on "
                      "channel='{}': {}",
                      j.value("code", std::string("?")), channel,
                      j.value("msg", "no reason"));
        }
        return;
    }

    if (!j.contains("arg") || !j.contains("data")) return;
    const std::string channel = j["arg"].value("channel", "");
    if (channel != "positions" && channel != "orders" &&
        channel != "account")
        return;
    // one line per frame so the 5-minute verification test can see which
    // channel actually delivered and what shape it arrived in, without
    // having to add logging later.
    LIVE_DBG("blofin: private frame channel={} data_is_array={}",
             channel, j["data"].is_array());

    // BloFin pushes `data` as an array on positions/orders, but the account
    // channel has been observed as a single object in some captures (the
    // REST balance reply is an object too). Normalise to an array here so
    // the per-channel loops below do not silently drop a valid frame.
    //
    // `rows` must outlive the loops below, so the object case is materialised
    // into a local array rather than bound to a temporary.
    const nlohmann::json single_wrap =
        j["data"].is_object() ? nlohmann::json::array({j["data"]})
                              : nlohmann::json::array();
    const nlohmann::json& rows =
        j["data"].is_array() ? j["data"] : single_wrap;
    if (rows.empty()) return;

    // tpsl_to_cancel is captured under the lock and fired after it releases:
    // a TP hit or a stop fire closes the position without going through
    // ClosePartial, so the TPSL is left armed against the next position.
    // cancel-tpsl is a REST call and must not run while state_mutex_ is held.
    std::string tpsl_to_cancel;
    {
    std::lock_guard<std::mutex> lock(state_mutex_);
    try {
        if (channel == "positions") {
            for (const auto& entry : rows) {
                if (!entry.is_object()) continue;
                if (entry.value("instId", "") != wire_symbol_) continue;
                // pending_entry_ is NOT cleared here: the venue pushes a
                // positions:"0" frame when an order is placed, before the
                // fill lands. Clearing on that frame leaves the guard down
                // while the entry is still resolving, and a second signal
                // walks straight into a second open position.
                //
                // `positions` is the documented field; `pos` is the
                // alternative spelling seen on some OKX-lineage venues. Read
                // whichever is present so a rename on the venue side does not
                // silently leave position_size_ at 0 while a real position is
                // open.
                const double sz = entry.contains("positions")
                                      ? JsonNum(entry, "positions")
                                      : JsonNum(entry, "pos");
                if (sz != 0.0) {
                    pending_entry_ = false;   // fill confirmed; entry resolved
                    // pending_entry_ms_ is deliberately NOT cleared here: the
                    // close-fill branch reads it to stamp TradeRecord::
                    // entry_time, and the close arrives after this frame.
                    position_size_ = std::abs(sz);
                    position_dir_  = (sz > 0.0) ? SignalDirection::BUY
                                                : SignalDirection::SELL;
                    entry_price_ = entry.contains("averagePrice")
                                       ? JsonNum(entry, "averagePrice")
                                       : JsonNum(entry, "avgPrice");
                    current_position_.direction   = position_dir_;
                    current_position_.volume      = position_size_;
                    current_position_.entry_price = entry_price_;
                    current_position_.stop_loss   = current_sl_;
                    current_position_.take_profit = current_tp_;
                } else {
                    position_size_   = 0.0;
                    position_dir_    = SignalDirection::NONE;
                    current_position_ = TradeSignal{};
                    // TP hit or liquidation: neither passes through
                    // ClosePartial, so the TPSL bookkeeping must be cleared
                    // here too. capture the id so the orphan TPSL can be
                    // cancelled after the lock releases; without this it
                    // fires on the next position at a stale price.
                    if (!current_tpsl_id_.empty())
                        tpsl_to_cancel = current_tpsl_id_;
                    current_tpsl_id_.clear();
                    entry_price_     = 0.0;
                    current_sl_      = 0.0;
                    current_tp_      = 0.0;
                    // the position is gone; the next entry stamps its own
                    // pending_entry_ms_. Clearing here keeps a stale entry
                    // time from leaking into a later record if the close
                    // fill never arrives (e.g. liquidation handled elsewhere).
                    pending_entry_ms_ = 0;
                }
            }
        } else if (channel == "account") {
            for (const auto& entry : rows) {
                if (!entry.is_object()) continue;
                // totalEquity is the aggregate; details[].equity is the
                // per-currency fallback for a frame that omits it. `equity`
                // is the alternative top-level spelling on some captures.
                double eq = JsonNum(entry, "totalEquity");
                if (!(eq > 0.0)) eq = JsonNum(entry, "equity");
                if (!(eq > 0.0) && entry.contains("details") &&
                    entry["details"].is_array()) {
                    for (const auto& row : entry["details"]) {
                        if (!row.is_object()) continue;
                        const double e = JsonNum(row, "equity");
                        if (e > 0.0) { eq = e; break; }
                    }
                }
                if (!std::isfinite(eq)) continue;
                current_balance_ = eq;
                if (eq > 0.0) {
                    LOG_INFO("BloFinBroker: account equity {}", eq);
                } else {
                    LOG_ERROR("BloFinBroker: account equity is {} -- no margin",
                              eq);
                }
            }
        } else if (channel == "orders") {
            for (const auto& entry : rows) {
                if (!entry.is_object()) continue;
                if (entry.value("instId", "") != wire_symbol_) continue;
                // pending_entry_ is NOT cleared here either: "live" arrives
                // before the fill, and treating it as resolution re-arms the
                // guard while the entry is still pending.
                const std::string state  = entry.value("state", "");
                // reduceOnly has been observed as both a JSON bool and a
                // string in venue captures. Reading it as a string only
                // (value("reduceOnly", "false")) throws on a bool and the
                // whole frame is dropped by the catch below, so a real close
                // never reaches trade_history_. Accept both spellings.
                bool reduce = false;
                if (entry.contains("reduceOnly")) {
                    const auto& r = entry["reduceOnly"];
                    if (r.is_boolean()) {
                        reduce = r.get<bool>();
                    } else if (r.is_string()) {
                        reduce = (r.get<std::string>() == "true");
                    }
                }
                const double filled = JsonNum(entry, "filledSize");
                if (state != "filled" || !(filled > 0.0)) continue;

                // A close can arrive as a plain reduceOnly order (our
                // ClosePartial), a TP/SL trigger, a trailing-stop fire, a
                // liquidation, or an ADL. Only the first carries
                // reduceOnly:"true" reliably; the rest are identified by
                // orderCategory. `pnl` is the safety net: per the docs it is
                // only meaningful on orders that close a position, so a
                // non-zero value means this fill closed something even if the
                // category is unfamiliar.
                const std::string category = entry.value("orderCategory", "");
                const double pnl = JsonNum(entry, "pnl");
                const bool is_close_order =
                    reduce ||
                    category == "tp" ||
                    category == "sl" ||
                    category == "adl" ||
                    category == "full_liquidation" ||
                    category == "partial_liquidation" ||
                    pnl != 0.0;
                if (is_close_order) {
                    last_closed_size_ = filled * contract_value_;
                    TradeRecord rec{};
                    // entry_time is the entry fill's updateTime, captured when
                    // the entry was sent. It used to be hardcoded 0, so every
                    // round-trip record carried a zero entry time and any
                    // consumer drawing a trade duration saw a 1970 start.
                    rec.entry_time   = pending_entry_ms_;
                    rec.exit_time    = static_cast<int64_t>(JsonNum(entry, "updateTime"));
                    // `side` on a close fill is the EXIT side. TradeRecord
                    // .direction means "which side was I on", so a sell that
                    // closed a long is a BUY record. This matches the
                    // signal.direction used on entry, so a round-trip record
                    // carries one direction, not two.
                    rec.direction    = (entry.value("side", "") == "sell")
                                           ? SignalDirection::BUY
                                           : SignalDirection::SELL;
                    rec.entry_price  = entry_price_;
                    rec.exit_price   = JsonNum(entry, "averagePrice");
                    rec.net_profit   = pnl;
                    rec.exit_reason  = "venue_fill";
                    trade_history_.push_back(rec);
                    net_profit_ += rec.net_profit;
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("BloFinBroker: bad field in '{}' frame: {}", channel, e.what());
    }
    }  // end scoped lock block

    // a TP hit (or a liquidation) closed the position without ClosePartial,
    // so the TPSL is still armed. cancel it outside the lock -- cancel-tpsl
    // is a REST call. a cancel of an already-triggered order returns code
    // "500" in data[0]; that is expected and not a failure worth escalating.
    if (!tpsl_to_cancel.empty()) {
        nlohmann::json cx = nlohmann::json::array(
            {{{"instId", wire_symbol_}, {"tpslId", tpsl_to_cancel}}});
        nlohmann::json cresp;
        const int cc = SendRestRequest("POST", "/api/v1/trade/cancel-tpsl",
                                       cx.dump(), &cresp);
        if (cc != 0 || AckCode(cresp) != 0) {
            LOG_WARN("BloFinBroker: cancel-tpsl {} rejected ({} / {}); "
                     "position already closed",
                     tpsl_to_cancel, cc, AckMessage(cresp));
        } else {
            LOG_INFO("BloFinBroker: cancelled orphan TPSL {} after position "
                     "closed", tpsl_to_cancel);
        }
    }
}

std::string BloFinBroker::GenerateSignature(const std::string& path_with_query,
                                            const std::string& method_upper,
                                            const std::string& timestamp_ms,
                                            const std::string& nonce,
                                            const std::string& body) {
    const std::string prehash =
        path_with_query + method_upper + timestamp_ms + nonce + body;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    HMAC(EVP_sha256(),
         api_secret_.data(), static_cast<int>(api_secret_.size()),
         reinterpret_cast<const unsigned char*>(prehash.data()), prehash.size(),
         digest, &dlen);
    // base64 of the ASCII hex STRING. not hex2bytes -- the docs call this out.
    return Base64Of(ToHex(digest, dlen));
}

int BloFinBroker::SendRestRequest(const std::string& method,
                                  const std::string& endpoint,
                                  const std::string& payload,
                                  nlohmann::json* out) {
    const std::string timestamp = std::to_string(NowSignedMillis());
    const std::string nonce = MakeNonce();
    std::string upper = method;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    const std::string sig =
        GenerateSignature(endpoint, upper, timestamp, nonce, payload);

    CURL* curl = curl_easy_init();
    if (!curl) return -1;
    const std::string url = rest_base_url_ + endpoint;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("ACCESS-KEY: " + api_key_).c_str());
    headers = curl_slist_append(headers, ("ACCESS-SIGN: " + sig).c_str());
    headers = curl_slist_append(headers, ("ACCESS-TIMESTAMP: " + timestamp).c_str());
    headers = curl_slist_append(headers, ("ACCESS-PASSPHRASE: " + api_passphrase_).c_str());
    headers = curl_slist_append(headers, ("ACCESS-NONCE: " + nonce).c_str());

    std::string response_body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    int ret_code = -1;
    const CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        LOG_ERROR("BloFinBroker: REST {} {} failed: {}", method, endpoint,
                  curl_easy_strerror(res));
    } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 429) {
            LOG_ERROR("HTTP 429 rate limited; sleeping 1 s");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        } else if (http_code < 200 || http_code >= 300) {
            LOG_ERROR("BloFinBroker: REST {} {} HTTP {}: {}", method, endpoint,
                      http_code, response_body);
        } else {
            const nlohmann::json j =
                nlohmann::json::parse(response_body, nullptr, false);
            if (out) *out = j;   // caller may inspect per-item code
            if (!j.is_discarded() && j.is_object() && j.contains("code")) {
                try {
                    ret_code = std::stoi(j["code"].is_string()
                                             ? j["code"].get<std::string>()
                                             : std::to_string(j["code"].get<int>()));
                } catch (...) { ret_code = -1; }
            }
        }
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ret_code;
}

bool BloFinBroker::SendRestGetTo(const std::string& base,
                                 const std::string& path,
                                 const std::string& query,
                                 nlohmann::json& out) {
    const std::string timestamp = std::to_string(NowSignedMillis());
    const std::string nonce = MakeNonce();
    const std::string signed_path = path + (query.empty() ? "" : "?" + query);
    const std::string sig =
        GenerateSignature(signed_path, "GET", timestamp, nonce, "");

    CURL* curl = curl_easy_init();
    if (!curl) return false;
    const std::string url = base + signed_path;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("ACCESS-KEY: " + api_key_).c_str());
    headers = curl_slist_append(headers, ("ACCESS-SIGN: " + sig).c_str());
    headers = curl_slist_append(headers, ("ACCESS-TIMESTAMP: " + timestamp).c_str());
    headers = curl_slist_append(headers, ("ACCESS-PASSPHRASE: " + api_passphrase_).c_str());
    headers = curl_slist_append(headers, ("ACCESS-NONCE: " + nonce).c_str());

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
        LOG_ERROR("BloFinBroker: GET {} failed: {}", path,
                  curl_easy_strerror(res));
    } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        out = nlohmann::json::parse(response_body, nullptr, false);
        if (out.is_discarded() || !out.is_object()) {
            LOG_ERROR("BloFinBroker: GET {} unparsable body", path);
        } else if (out.value("code", std::string("-1")) != "0") {
            LOG_ERROR("BloFinBroker: GET {} code {} {}", path,
                      out.value("code", "?"), out.value("msg", ""));
        } else {
            ok = true;
        }
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

bool BloFinBroker::SendRestGet(const std::string& path, const std::string& query,
                               nlohmann::json& out) {
    return SendRestGetTo(rest_base_url_, path, query, out);
}

bool BloFinBroker::SendMarketGet(const std::string& path,
                                 const std::string& query,
                                 nlohmann::json& out) {
    if (SendRestGetTo(rest_base_url_, path, query, out)) return true;
    if (market_rest_base_.empty() || market_rest_base_ == rest_base_url_)
        return false;
    LOG_WARN("BloFinBroker: {} not served by {}; retrying {}",
             path, rest_base_url_, market_rest_base_);
    return SendRestGetTo(market_rest_base_, path, query, out);
}

double BloFinBroker::GetBalance() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_balance_;
}

double BloFinBroker::GetEquity() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_balance_;
}

bool BloFinBroker::PendingEntryLocked() const {
    if (!pending_entry_) return false;
    if (NowMillis() - pending_since_ms_ > kPendingEntryTimeoutMs) {
        pending_entry_ = false;
        return false;
    }
    return true;
}

bool BloFinBroker::HasOpenPosition() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return position_size_ > 0.0 || PendingEntryLocked();
}

TradeSignal BloFinBroker::GetCurrentPosition() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_position_;
}

double BloFinBroker::GetNetProfit() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return net_profit_;
}

std::vector<TradeRecord> BloFinBroker::GetTradeHistory() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return trade_history_;
}

int64_t BloFinBroker::GetLastTradeTime() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return trade_history_.empty() ? 0 : trade_history_.back().exit_time;
}

size_t BloFinBroker::GetTradeCount() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return trade_history_.size();
}

bool BloFinBroker::IsConnected() const {
    return ws_authed_.load(std::memory_order_acquire);
}

double BloFinBroker::GetLastClosedSize() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_closed_size_;
}

void BloFinBroker::ProcessSignal(TradeSignal signal, double fill_price,
                                 int64_t current_time) {
    (void)current_time;
    if (signal.volume <= 0.0 || signal.direction == SignalDirection::NONE)
        return;
    if (!rules_.valid || !(contract_value_ > 0.0)) {
        LOG_ERROR("BloFinBroker: no instrument rules; refusing to send");
        return;
    }

    // signal.volume is BTC. the venue's `size` is CONTRACTS.
    const double contracts =
        std::floor((signal.volume / contract_value_) / rules_.qty_step + 1e-9)
        * rules_.qty_step;
    if (!(contracts > 0.0)) {
        LOG_ERROR("BloFinBroker: volume {} BTC < one lot", signal.volume);
        return;
    }
    if (contracts < rules_.min_qty) {
        LOG_ERROR("BloFinBroker: {} contracts < minSize {}",
                  contracts, rules_.min_qty);
        return;
    }

    const bool is_buy = signal.direction == SignalDirection::BUY;
    nlohmann::json order = {
        {"instId",        wire_symbol_},
        {"marginMode",    "cross"},
        {"positionSide",  "net"},
        {"side",          is_buy ? "buy" : "sell"},
        {"orderType",     "market"},
        {"size",          FormatStep(contracts, rules_.qty_step)},
        {"clientOrderId", mokuren::blofin::MakeClientOrderId(
                              "mok", NowSignedMillis(), ++order_seq_)},
    };

    LIVE_DBG("blofin: order {}", order.dump());
    nlohmann::json entry_resp;
    const int ret = SendRestRequest("POST", "/api/v1/trade/order",
                                    order.dump(), &entry_resp);
    if (ret != 0 || AckCode(entry_resp) != 0) {
        LOG_ERROR("BloFinBroker: entry rejected ({} / {}) -- not trading it",
                  ret, AckMessage(entry_resp));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        pending_entry_       = true;
        pending_since_ms_    = NowMillis();
        // the entry fill's updateTime is not carried on the positions frame,
        // so stamp it here, at the moment the entry was accepted. The close
        // fill branch reads this to fill TradeRecord::entry_time.
        pending_entry_ms_    = pending_since_ms_;
        if (signal.stop_loss   > 0.0) current_sl_ = signal.stop_loss;
        if (signal.take_profit > 0.0) current_tp_ = signal.take_profit;
    }

    const bool want_tp = signal.take_profit > 0.0;
    const bool want_sl = signal.stop_loss   > 0.0;
    if (!want_tp && !want_sl) return;

    // TP/SL order. Both legs go in one order-tpsl, the same shape Bybit's
    // entry carries as stopLoss + takeProfit: the hard stop lives on the
    // venue's matching engine, placed as a second call immediately after the
    // entry ack (the gap is bounded by the emergency-close fallback below,
    // not by the venue), and PM's ladder moves it later via amend-tpsl (see
    // UpdateStopLoss).
    //
    // why split, not one call: blofin's /trade/order does accept attached
    // slTriggerPrice/tpTriggerPrice, but the only way to move that stop
    // afterwards is /trade/amend-order, whose docs scope it to "a single
    // LIMIT order". our entries are market orders that fill immediately, so
    // amend-order on a filled entry is not guaranteed to accept a stop move
    // -- and a silently-static trail is a worse failure than a half-second
    // unfilled window. amend-tpsl has no such wording, so the standalone
    // TP/SL order is the shape that keeps the trail path guaranteed.
    nlohmann::json tpsl = {
        {"instId",       wire_symbol_},
        {"marginMode",   "cross"},
        {"positionSide", "net"},
        {"side",         is_buy ? "sell" : "buy"},
        {"size",         "-1"},
        {"reduceOnly",   "true"},
    };
    if (want_tp) {
        const double tp = RoundToStep(signal.take_profit, rules_.tick_size);
        tpsl["tpTriggerPrice"]     = FormatStep(tp, rules_.tick_size);
        tpsl["tpOrderPrice"]       = "-1";
        tpsl["tpTriggerPriceType"] = "last";
    }
    if (want_sl) {
        const double sl = RoundToStep(signal.stop_loss, rules_.tick_size);
        tpsl["slTriggerPrice"]     = FormatStep(sl, rules_.tick_size);
        tpsl["slOrderPrice"]       = "-1";
        tpsl["slTriggerPriceType"] = "last";
    }
    LIVE_DBG("blofin: order {}", tpsl.dump());
    nlohmann::json resp;
    const int tcode = SendRestRequest("POST", "/api/v1/trade/order-tpsl",
                                      tpsl.dump(), &resp);
    if (tcode != 0 || AckCode(resp) != 0) {
        LOG_ERROR("BloFinBroker: TP/SL rejected ({} / {}); entry is "
                  "UNPROTECTED -- closing to stay safe",
                  tcode, AckMessage(resp));
        // ClosePartial cannot be used here: position_size_ is still 0
        // because the venue's `positions` ws frame has not landed yet
        // (the entry ack came back, the fill push is 50-500 ms behind).
        // ClosePartial would see position_size_ <= 0 and no-op, leaving
        // the entry open with no protection. Send the reduce directly
        // against the size we just sent; net-mode reduceOnly caps any
        // overshoot from a partial fill, so this cannot flip the
        // position. The ws `positions` frame will set position_size_ = 0
        // a moment later and bookkeeping self-heals.
        nlohmann::json emergency = {
            {"instId",        wire_symbol_},
            {"marginMode",    "cross"},
            {"positionSide",  "net"},
            {"side",          is_buy ? "sell" : "buy"},
            {"orderType",     "market"},
            {"size",          FormatStep(contracts, rules_.qty_step)},
            {"reduceOnly",    "true"},
            {"clientOrderId", mokuren::blofin::MakeClientOrderId(
                                  "emg", NowSignedMillis(), ++order_seq_)},
        };
        LIVE_DBG("blofin: order {}", emergency.dump());
        nlohmann::json eresp;
        const int ecode = SendRestRequest("POST", "/api/v1/trade/order",
                                          emergency.dump(), &eresp);
        if (ecode != 0 || AckCode(eresp) != 0) {
            LOG_ERROR("BloFinBroker: EMERGENCY CLOSE FAILED ({} / {}); "
                      "position is OPEN and UNPROTECTED -- intervene "
                      "manually",
                      ecode, AckMessage(eresp));
        }
        return;
    }
    std::string tpsl_id;
    if (resp.contains("data") && resp["data"].is_object())
        tpsl_id = resp["data"].value("tpslId", std::string{});
    if (!tpsl_id.empty()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_tpsl_id_ = tpsl_id;
    }
}

void BloFinBroker::CheckRisk(double, int64_t) {
    // intentionally empty. PositionManager owns the management ladder.
}

void BloFinBroker::UpdateStopLoss(double new_sl) {
    if (!(new_sl > 0.0) || !(rules_.tick_size > 0.0)) return;
    const int64_t now = NowMillis();
    if (last_sl_move_ms_ != 0 && now - last_sl_move_ms_ < kMinSlMoveIntervalMs) {
        LIVE_DBG("blofin: debounced stop move");
        return;
    }
    last_sl_move_ms_ = now;

    const double rounded = RoundToStep(new_sl, rules_.tick_size);
    const std::string sl_str = FormatStep(rounded, rules_.tick_size);

    bool have_tpsl = false;
    std::string tpsl_id;
    // the direction is written by the private websocket thread, so it must be
    // read under the same lock as the tpsl id. read unsynchronised it can land
    // between the position_size_ write and the position_dir_ write in the
    // positions handler, still hold NONE, and place a fresh stop on the WRONG
    // side -- which the venue rejects, or worse accepts as a reduceOnly order
    // pointing the other way.
    SignalDirection dir = SignalDirection::NONE;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        have_tpsl = !current_tpsl_id_.empty();
        tpsl_id   = current_tpsl_id_;
        dir       = position_dir_;
    }
    if (!have_tpsl) {
        RefreshTpslId();
        std::lock_guard<std::mutex> lock(state_mutex_);
        have_tpsl = !current_tpsl_id_.empty();
        tpsl_id   = current_tpsl_id_;
        dir       = position_dir_;
    }

    if (!have_tpsl) {
        // no TPSL exists yet: place one with only the SL leg.
        //
        // `side` is the EXIT side, so an unknown direction is not something to
        // guess at: refuse rather than send protection that points the wrong
        // way. the next tick retries with a direction the ws handler has
        // committed.
        if (dir == SignalDirection::NONE) {
            LOG_ERROR("BloFinBroker: cannot place a fresh TPSL for {}: "
                      "position direction is unknown", wire_symbol_);
            return;
        }
        nlohmann::json tpsl = {
            {"instId",            wire_symbol_},
            {"marginMode",        "cross"},
            {"positionSide",      "net"},
            {"side",              dir == SignalDirection::BUY ? "sell" : "buy"},
            {"size",              "-1"},
            {"reduceOnly",        "true"},
            {"slTriggerPrice",     sl_str},
            {"slTriggerPriceType", "last"},
        };
        nlohmann::json fresh_resp;
        const int c = SendRestRequest("POST", "/api/v1/trade/order-tpsl",
                                      tpsl.dump(), &fresh_resp);
        if (c != 0 || AckCode(fresh_resp) != 0) {
            LOG_ERROR("BloFinBroker: fresh TPSL rejected ({} / {}); position "
                      "remains unprotected", c, AckMessage(fresh_resp));
            return;
        }
        RefreshTpslId();
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_sl_ = rounded;
        // mirror the amend path below: PositionManager reads
        // GetCurrentPosition().stop_loss to confirm a move landed, and a
        // fresh-TPSL placement that only updated current_sl_ left that field
        // stale for one frame -- long enough for the ladder to re-send the
        // same move or skip a retry it should have made.
        current_position_.stop_loss = rounded;
        return;
    }

    nlohmann::json amend = {
        {"instId",                wire_symbol_},
        {"tpslId",                tpsl_id},
        {"newSlTriggerPrice",     sl_str},
        {"newSlTriggerPriceType", "last"},
    };
    nlohmann::json resp;
    const int c = SendRestRequest("POST", "/api/v1/trade/amend-tpsl",
                                  amend.dump(), &resp);
    if (c != 0 || AckCode(resp) != 0) {
        LOG_ERROR("BloFinBroker: amend-tpsl rejected ({} / {}); SL NOT moved",
                  c, AckMessage(resp));
        return;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_sl_ = rounded;
    current_position_.stop_loss = rounded;
}

bool BloFinBroker::RefreshTpslId() {
    nlohmann::json j;
    if (!SendRestGet("/api/v1/trade/orders-tpsl-pending",
                     "instId=" + wire_symbol_, j)) return false;
    if (!j.contains("data") || !j["data"].is_array()) return false;
    for (const auto& t : j["data"]) {
        if (!t.is_object()) continue;
        if (t.value("instId", "") != wire_symbol_) continue;
        const std::string id = t.value("tpslId", "");
        if (id.empty()) continue;
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_tpsl_id_ = id;
        return true;
    }
    return false;
}

void BloFinBroker::ClosePartial(double fraction, double, int64_t,
                                const std::string& reason) {
    if (fraction <= 0.0) return;
    if (fraction > 1.0) fraction = 1.0;
    if (!rules_.valid || !(contract_value_ > 0.0)) return;

    double close_contracts = 0.0;
    SignalDirection dir = SignalDirection::NONE;
    double size_before = 0.0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (position_size_ <= 0.0) return;
        if (position_dir_ == SignalDirection::NONE) {
            LOG_ERROR("BloFinBroker: refusing to reduce with unknown direction");
            return;
        }
        // position_size_ is in CONTRACTS (matches the venue's "positions"
        // field). Do not divide by contract_value_ here -- that would size the
        // reduce order 1000x too large on BTC-USDT.
        close_contracts =
            std::floor((position_size_ * fraction) / rules_.qty_step + 1e-9)
            * rules_.qty_step;
        if (!(close_contracts > 0.0)) return;
        dir = position_dir_;
        size_before = position_size_;
    }

    const bool closing_long = dir == SignalDirection::BUY;
    nlohmann::json payload = {
        {"instId",        wire_symbol_},
        {"marginMode",    "cross"},
        {"positionSide",  "net"},
        {"side",          closing_long ? "sell" : "buy"},
        {"orderType",     "market"},
        {"size",          FormatStep(close_contracts, rules_.qty_step)},
        {"reduceOnly",    "true"},
        {"clientOrderId", mokuren::blofin::MakeClientOrderId(
                              "close", NowSignedMillis(), ++order_seq_)},
    };

    LOG_INFO("BloFinBroker: closing {} contracts ({})", close_contracts, reason);
    LIVE_DBG("blofin: order {}", payload.dump());
    nlohmann::json close_resp;
    const int ret = SendRestRequest("POST", "/api/v1/trade/order",
                                    payload.dump(), &close_resp);
    if (ret != 0 || AckCode(close_resp) != 0) {
        // the protective orders are still armed: the cancel block below only
        // runs after a successful close, so a rejected close leaves the
        // position protected rather than naked.
        LOG_ERROR("BloFinBroker: close rejected ({} / {}); unchanged",
                  ret, AckMessage(close_resp));
        return;
    }

    // A full close makes the pending TPSL stale: it has size "-1" and would
    // re-fire on the next position. A partial close is fine -- it re-sizes
    // against the new position at trigger time.
    //
    // This runs AFTER the close ack, not before: cancelling first would leave
    // the position naked if the close order were then rejected.
    if (fraction >= 1.0) {
        std::string tid;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            tid = current_tpsl_id_;
        }
        if (!tid.empty()) {
            // cancel-tpsl takes an array per the docs.
            nlohmann::json cx = nlohmann::json::array(
                {{{"instId", wire_symbol_}, {"tpslId", tid}}});
            nlohmann::json cresp;
            const int cc = SendRestRequest("POST", "/api/v1/trade/cancel-tpsl",
                                           cx.dump(), &cresp);
            if (cc == 0 && AckCode(cresp) == 0) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (current_tpsl_id_ == tid) current_tpsl_id_.clear();
            } else {
                // leave the id in place; the sz==0 frame handler will retry
                LOG_WARN("BloFinBroker: cancel-tpsl {} rejected ({} / {}); "
                         "will retry on position close",
                         tid, cc, AckMessage(cresp));
            }
        }
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    // last_closed_size_ stays in BASE units (matches GetLastClosedSize's
    // consumer). position_size_ stays in CONTRACTS.
    last_closed_size_ = close_contracts * contract_value_;
    if (std::abs(position_size_ - size_before) >= 1e-9) return;
    position_size_ = std::max(0.0, position_size_ - close_contracts);
    current_position_.volume = position_size_;
    if (position_size_ == 0.0) {
        position_dir_ = SignalDirection::NONE;
        current_position_.direction = SignalDirection::NONE;
    }
}
