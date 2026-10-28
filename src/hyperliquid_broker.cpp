#include "hyperliquid_broker.hpp"

#include "live_debug.hpp"
#include "logger.hpp"

#include <chrono>
#include <exception>
#include <iomanip>
#include <sstream>
#include <string>

namespace {

// hyperliquid identifies perps by index, not by name, and the index is only
// valid for the venue's current asset ordering. 0 is BTC on the perp venue;
// a multi-symbol setup must resolve this from the /info "meta" endpoint
// instead of hardcoding it (same caveat as BybitBroker's hardcoded BTCUSDT).
constexpr int kBtcAssetIndex = 0;
constexpr const char* kBtcCoin = "BTC";

int64_t NowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// hyperliquid mixes decimal strings and raw numbers for the same field across
// channels, so both are accepted; anything else (null, bool, object) yields
// the fallback rather than a type_error.
double NumField(const nlohmann::json& obj, const char* key, double fallback = 0.0) {
    if (!obj.is_object() || !obj.contains(key)) return fallback;
    const auto& v = obj[key];
    try {
        if (v.is_string()) return std::stod(v.get<std::string>());
        if (v.is_number()) return v.get<double>();
    } catch (const std::exception&) {
        return fallback;
    }
    return fallback;
}

std::string StrField(const nlohmann::json& obj, const char* key,
                     const std::string& fallback = "") {
    if (!obj.is_object() || !obj.contains(key)) return fallback;
    const auto& v = obj[key];
    if (v.is_string()) return v.get<std::string>();
    return fallback;
}

// Formats a price/size for a JSON body. Hyperliquid rejects values with
// trailing zeros, so this emits the full value with no padding; the ostream
// default of 6 significant digits would round 65000.123456 down to 65000.1.
std::string FormatDouble(double value) {
    std::ostringstream oss;
    oss << std::setprecision(15) << value;
    return oss.str();
}

} // namespace

HyperliquidBroker::HyperliquidBroker(const std::string& wallet_address,
                                     const std::string& private_key,
                                     const std::string& private_ws_url,
                                     const std::string& rest_url)
    : wallet_address_(wallet_address),
      private_key_(private_key),
      private_ws_url_(private_ws_url),
      rest_url_(rest_url) {
    ConnectPrivateStream();
}

HyperliquidBroker::~HyperliquidBroker() {
    // stop() joins the websocket thread before members are destroyed.
    private_ws_.stop();
}

void HyperliquidBroker::ConnectPrivateStream() {
    private_ws_.setUrl(private_ws_url_);

    // stale account state is dangerous (the engine would size the next order
    // against a position that no longer exists), so reconnect automatically.
    private_ws_.enableAutomaticReconnection();
    private_ws_.setPingInterval(30);

    private_ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
            case ix::WebSocketMessageType::Open:
                // the subscription is only valid once the handshake completed.
                // Open fires again on every reconnect, so the state feed
                // re-subscribes automatically.
                SendSubscription();
                ws_connected_ = true;
                break;
            case ix::WebSocketMessageType::Message: {
                // parse without throwing: malformed frames are dropped instead
                // of tearing down the websocket thread.
                const nlohmann::json frame =
                    nlohmann::json::parse(msg->str, nullptr, false);
                if (frame.is_discarded() || !frame.is_object()) {
                    LIVE_DBG("hyperliquid: dropped unparsable private frame");
                    break;
                }
                LIVE_DBG("hyperliquid: private frame {}", msg->str);
                HandlePrivateMessage(frame);
                break;
            }
            case ix::WebSocketMessageType::Error:
            case ix::WebSocketMessageType::Close:
                ws_connected_ = false;
                break;
            default:
                break;
        }
    });

    private_ws_.start();
}

void HyperliquidBroker::SendSubscription() {
    if (wallet_address_.empty()) {
        LOG_ERROR("HyperliquidBroker: no wallet address configured, "
                  "cannot subscribe to webData2");
        return;
    }

    // {"method":"subscribe","subscription":{"type":"webData2","user":"0x..."}}
    // built from wallet_address_ rather than a literal so the address cannot
    // break the JSON. nlohmann orders keys alphabetically, which differs
    // cosmetically from the hand-written form but is the same document.
    nlohmann::json sub;
    sub["method"] = "subscribe";
    sub["subscription"]["type"] = "webData2";
    sub["subscription"]["user"] = wallet_address_;

    private_ws_.send(sub.dump());
}

void HyperliquidBroker::HandlePrivateMessage(const nlohmann::json& payload) {
    if (!payload.is_object()) return;

    // webData2 is the only user-state channel subscribed to.
    if (payload.value("channel", "") != "webData2") return;
    if (!payload.contains("data") || !payload["data"].is_object()) return;

    const auto& data = payload["data"];

    // One lock for the whole handler: every field written below is guarded by
    // state_mutex_ and must be published as a consistent snapshot.
    std::lock_guard<std::mutex> lock(state_mutex_);

    // a malformed value must not crash the websocket thread.
    try {
        if (data.contains("clearinghouseState") && data["clearinghouseState"].is_object()) {
            const auto& chs = data["clearinghouseState"];

            // ---- account value ----
            if (chs.contains("marginSummary") && chs["marginSummary"].is_object()) {
                current_balance_ =
                    NumField(chs["marginSummary"], "accountValue", current_balance_);
            }

            // ---- open positions ----
            // recomputed from scratch on every push: a partial update would
            // leave position_size_ stuck at its previous value after a close.
            unrealized_pnl_ = 0.0;
            position_size_ = 0.0;
            current_position_.direction = SignalDirection::NONE;
            current_position_.volume = 0.0;

            if (chs.contains("assetPositions") && chs["assetPositions"].is_array()) {
                for (const auto& entry : chs["assetPositions"]) {
                    if (!entry.is_object() || !entry.contains("position") ||
                        !entry["position"].is_object()) {
                        continue;
                    }
                    const auto& pos = entry["position"];
                    if (StrField(pos, "coin") != kBtcCoin) continue;

                    // "szi" is signed: positive = long, negative = short.
                    const double szi = NumField(pos, "szi");
                    unrealized_pnl_ += NumField(pos, "unrealizedPnl");
                    if (szi == 0.0) continue;

                    entry_price_ = NumField(pos, "entryPx", entry_price_);
                    position_size_ = (szi < 0.0) ? -szi : szi;

                    current_position_.direction =
                        (szi > 0.0) ? SignalDirection::BUY : SignalDirection::SELL;
                    current_position_.volume = position_size_;
                    current_position_.entry_price = entry_price_;
                    current_position_.stop_loss = current_sl_;
                    current_position_.take_profit = current_tp_;
                }
            }
        }

        // ---- resting stop / take-profit trigger orders ----
        // hyperliquid keeps SL/TP as separate reduce-only trigger orders rather
        // than as fields on the position, so they are read back from openOrders.
        // best effort: the venue may also hold non-positional triggers.
        if (data.contains("openOrders") && data["openOrders"].is_array()) {
            for (const auto& order : data["openOrders"]) {
                if (!order.is_object()) continue;
                if (StrField(order, "coin") != kBtcCoin) continue;
                if (!order.value("isPositionTpsl", false)) continue;

                const double trigger_px = NumField(order, "triggerPx");
                if (trigger_px <= 0.0) continue;

                const std::string tpsl = StrField(order, "tpsl");
                if (tpsl == "sl") {
                    current_sl_ = trigger_px;
                } else if (tpsl == "tp") {
                    current_tp_ = trigger_px;
                }
            }
        }

        // ---- fills: realized pnl and closed trade records ----
        if (data.contains("fills") && data["fills"].is_array()) {
            for (const auto& fill : data["fills"]) {
                if (!fill.is_object()) continue;
                if (StrField(fill, "coin") != kBtcCoin) continue;

                // "dir" is one of Open Long / Close Long / Open Short /
                // Close Short, or "<side> > <side>" when a fill flips the
                // position in one go.
                const std::string dir = StrField(fill, "dir");
                const bool is_flip = dir.find('>') != std::string::npos;
                const bool closes = is_flip || dir.rfind("Close", 0) == 0;
                const bool opens = is_flip || dir.rfind("Open", 0) == 0;
                if (!closes && !opens) continue;

                const double size = NumField(fill, "sz");
                const double price = NumField(fill, "px");
                const double closed_pnl = NumField(fill, "closedPnl");
                const int64_t time =
                    (fill.contains("time") && fill["time"].is_number())
                        ? fill["time"].get<int64_t>()
                        : NowMillis();

                if (closes) {
                    realized_pnl_ += closed_pnl;
                    last_closed_size_ = size;

                    // entry_time/entry_price come from the cached position:
                    // webData2 pushes the fill, not the opening leg.
                    TradeRecord rec{};
                    rec.entry_time = time;
                    rec.exit_time = time;
                    rec.direction = (dir.find("Long") != std::string::npos)
                                        ? SignalDirection::BUY
                                        : SignalDirection::SELL;
                    rec.entry_price = entry_price_;
                    rec.exit_price = price;
                    rec.net_profit = closed_pnl;
                    rec.exit_reason = dir;
                    trade_history_.push_back(std::move(rec));
                }

                if (opens) {
                    entry_price_ = price;
                    current_position_.entry_price = price;
                }
            }
        }

        // realized + open unrealized pnl, relative to process start.
        net_profit_ = realized_pnl_ + unrealized_pnl_;
    } catch (const std::exception& e) {
        LOG_WARN("HyperliquidBroker: bad field in webData2 frame: {}", e.what());
    }
}

std::string HyperliquidBroker::SignOrderEIP712(const nlohmann::json& order_payload) {
    // ---------------------------------------------------------------------
    // PLACEHOLDER -- no key material is touched until the crypto is linked.
    //
    // Signing a hyperliquid action is a two-stage hash:
    //   1. keccak256 over the msgpack encoding of the action, prefixed with a
    //      marker byte, to derive the "phantom agent" struct
    //   2. an EIP-712 typed-data hash over that struct, signed with the
    //      wallet's secp256k1 private key
    // The result is the r||s||v triple, which the /exchange request carries as
    // {"signature":{"r":"0x..","s":"0x..","v":27}}.
    //
    // Hooking this up needs:
    //   * libsecp256k1 for the ECDSA step (link it in CMakeLists.txt)
    //   * a keccak256 implementation (not SHA3-256 -- the padding differs)
    //   * the EIP-712 domain ("Exchange", version "1", chainId 1337) and the
    //     type hashes for the Order/Cancel/Agent structs, whose fields must be
    //     sorted alphabetically before hashing
    // ---------------------------------------------------------------------
    (void)order_payload;
    LOG_WARN("HyperliquidBroker: SignOrderEIP712 is a stub -- crypto not linked, "
             "returning an empty signature");
    return std::string();
}

// ---- cached state getters (served without blocking) ----

double HyperliquidBroker::GetBalance() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_balance_;
}

double HyperliquidBroker::GetEquity() const {
    // accountValue from webData2's marginSummary IS the portfolio value:
    // equity including the open position's unrealized pnl. HandlePrivateMessage
    // already folds that leg into current_balance_ via the accountValue field
    // — adding unrealized_pnl_ here would count the same pnl twice. this
    // mirrors BybitBroker::GetEquity, where the wallet push's totalEquity
    // plays the same role: the venue is the source of truth for what the
    // account is worth right now, and the prop guard must read that number
    // and not one derived from a local mark.
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_balance_;
}

bool HyperliquidBroker::HasOpenPosition() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return position_size_ > 0.0;
}

TradeSignal HyperliquidBroker::GetCurrentPosition() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_position_;
}

double HyperliquidBroker::GetNetProfit() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return net_profit_;
}

std::vector<TradeRecord> HyperliquidBroker::GetTradeHistory() const {
    // the copy is taken while the lock is held, so the caller receives a
    // consistent snapshot instead of a reference into a vector the websocket
    // thread may be appending to.
    std::lock_guard<std::mutex> lock(state_mutex_);
    return trade_history_;
}

int64_t HyperliquidBroker::GetLastTradeTime() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return trade_history_.empty() ? 0 : trade_history_.back().exit_time;
}

size_t HyperliquidBroker::GetTradeCount() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return trade_history_.size();
}

double HyperliquidBroker::GetLastClosedSize() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_closed_size_;
}

// ---- orchestration hooks ----

void HyperliquidBroker::ProcessSignal(TradeSignal signal, double fill_price,
                                      int64_t current_time) {
    if (signal.volume <= 0.0 || signal.direction == SignalDirection::NONE) return;

    const bool is_buy = (signal.direction == SignalDirection::BUY);

    // ---------------------------------------------------------------------
    // HYPERLIQUID ORDER ACTION
    //
    // POST /exchange expects:
    //   {"action": {...}, "nonce": <ms>, "signature": {"r","s","v"}}
    //
    // hyperliquid has no stopLoss/takeProfit field on the entry order, so the
    // protective levels are submitted as separate reduce-only trigger orders in
    // the same action, which is what "grouping": "normalTpsl" expresses.
    //
    //   a = asset index, b = isBuy, p = limit price, s = size, r = reduceOnly
    //   t = {"limit": {"tif": "Ioc"}} for the entry
    //       {"trigger": {"triggerPx": .., "isMarket": true, "tpsl": "sl"|"tp"}}
    //       for the protective legs
    //
    // A market entry is expressed as a limit at the estimated fill price with
    // an immediate-or-cancel time in force.
    // ---------------------------------------------------------------------
    const double entry_px = (fill_price > 0.0) ? fill_price : signal.entry_price;

    nlohmann::json entry;
    entry["a"] = kBtcAssetIndex;
    entry["b"] = is_buy;
    entry["p"] = FormatDouble(entry_px);
    entry["s"] = FormatDouble(signal.volume);
    entry["r"] = false;
    entry["t"]["limit"]["tif"] = "Ioc";

    nlohmann::json orders = nlohmann::json::array();
    orders.push_back(std::move(entry));

    if (signal.stop_loss > 0.0) {
        nlohmann::json sl;
        sl["a"] = kBtcAssetIndex;
        sl["b"] = !is_buy;
        sl["p"] = FormatDouble(signal.stop_loss);
        sl["s"] = FormatDouble(signal.volume);
        sl["r"] = true;
        sl["t"]["trigger"]["triggerPx"] = FormatDouble(signal.stop_loss);
        sl["t"]["trigger"]["isMarket"] = true;
        sl["t"]["trigger"]["tpsl"] = "sl";
        orders.push_back(std::move(sl));
    }

    if (signal.take_profit > 0.0) {
        nlohmann::json tp;
        tp["a"] = kBtcAssetIndex;
        tp["b"] = !is_buy;
        tp["p"] = FormatDouble(signal.take_profit);
        tp["s"] = FormatDouble(signal.volume);
        tp["r"] = true;
        tp["t"]["trigger"]["triggerPx"] = FormatDouble(signal.take_profit);
        tp["t"]["trigger"]["isMarket"] = true;
        tp["t"]["trigger"]["tpsl"] = "tp";
        orders.push_back(std::move(tp));
    }

    nlohmann::json request;
    request["action"]["type"] = "order";
    request["action"]["orders"] = orders;
    request["action"]["grouping"] = (orders.size() > 1) ? "normalTpsl" : "na";
    request["nonce"] = (current_time > 0) ? current_time : NowMillis();

    const std::string signature = SignOrderEIP712(request);
    if (signature.empty()) {
        // signing is not linked yet, so the request would be rejected.
        request["signature"] = nullptr;
        LOG_WARN("HyperliquidBroker: NOT submitting -- order signing is not linked yet");
    } else {
        // NOTE: hyperliquid wants an {r, s, v} object; the stub returns the
        // concatenated hex, which still has to be split into its 32/32/1 parts.
        request["signature"] = signature;
    }

    // No live REST call yet: the signature above is a stub, so a POST to
    // {rest_url_}/exchange would only fail.
    LOG_INFO("hyperliquid: would POST {}{} with {}", rest_url_, "/exchange",
             request.dump());
}

void HyperliquidBroker::CheckRisk(double current_price, int64_t current_time) {
    // DYNAMIC RISK MANAGEMENT (client side, mirrors PaperTrader/BybitBroker):
    // the hard stop already sits on the exchange as a trigger order, but
    // break-even and trailing adjustments are computed locally because
    // hyperliquid's native triggers cannot express the strategy's
    // percentage-of-entry rules.
    //
    //   1. track the running high/low since entry for both directions
    //   2. if unrealized profit >= be_trigger_pct -> move SL to entry +/- be_target_pct
    //   3. if unrealized profit >= trailing_trigger_pct -> keep SL
    //      trailing_dist_pct behind the best observed price
    //   4. if the computed SL differs from the last one pushed, call
    //      UpdateStopLoss(new_sl)
    //
    // State is read from the webData2 snapshots so this runs without a
    // round-trip to the exchange.
    (void)current_price;
    (void)current_time;
}

void HyperliquidBroker::UpdateStopLoss(double new_sl) {
    // Persist the new stop first so CheckRisk can diff against it.
    double volume = 0.0;
    SignalDirection direction = SignalDirection::NONE;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_sl_ = new_sl;
        volume = current_position_.volume;
        direction = current_position_.direction;
    }

    if (volume <= 0.0 || direction == SignalDirection::NONE) return;

    nlohmann::json order;
    order["a"] = kBtcAssetIndex;
    order["b"] = (direction != SignalDirection::BUY);
    order["p"] = FormatDouble(new_sl);
    order["s"] = FormatDouble(volume);
    order["r"] = true;
    order["t"]["trigger"]["triggerPx"] = FormatDouble(new_sl);
    order["t"]["trigger"]["isMarket"] = true;
    order["t"]["trigger"]["tpsl"] = "sl";

    // TODO(hyperliquid): replacing a resting stop is a cancel + place pair --
    //   {"action":{"type":"cancel","cancels":[{"a":<asset>,"o":<oid>}]}}
    // followed by the reduce-only trigger order below. The resting order's oid
    // is not cached yet (HandlePrivateMessage only tracks triggerPx), so the
    // cancel cannot be built. Signing is also still a stub.
    LOG_WARN("HyperliquidBroker: NOT submitting stop move -- signing not linked yet "
             "and the resting order id is not tracked");
    LOG_INFO("hyperliquid: would replace stop with {}", order.dump());
}

void HyperliquidBroker::ClosePartial(double fraction, double current_price,
                                     int64_t current_time,
                                     const std::string& reason) {
    if (fraction <= 0.0 || fraction > 1.0) return;

    double volume = 0.0;
    SignalDirection direction = SignalDirection::NONE;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        volume = current_position_.volume;
        direction = current_position_.direction;
    }

    if (volume <= 0.0 || direction == SignalDirection::NONE) return;

    const double close_size = volume * fraction;

    // reduce-only market order for `fraction` of the open position.
    nlohmann::json order;
    order["a"] = kBtcAssetIndex;
    order["b"] = (direction != SignalDirection::BUY);
    order["p"] = FormatDouble(current_price);
    order["s"] = FormatDouble(close_size);
    order["r"] = true;
    order["t"]["limit"]["tif"] = "Ioc";

    LOG_WARN("HyperliquidBroker: NOT submitting partial close -- signing not linked yet");
    LOG_INFO("hyperliquid: would close {} of {} ({}) with {}", close_size, volume,
             reason, order.dump());
}
