#pragma once

#include "ibroker.hpp"
#include "venue_spec.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Live execution adapter for the BloFin V5 API (swap).
//
// Shape mirrors BybitBroker on purpose: same cached-state getters, same
// EstablishStartupTruth / SyncClock / FetchInstrumentRules / ReconcileAccount
// sequence, same private websocket + heartbeat, same pending_entry_ guard.
// The differences are all in the wire format:
//
//   - three credentials (adds a passphrase)
//   - base64 HMAC instead of hex
//   - /api/v1/... paths
//   - SL/TP are placed as a SEPARATE TP/SL order (POST /api/v1/trade/order-tpsl),
//     not attached to the entry. this is the one behavioural difference that
//     matters: the hard stop is not on the matching engine until the second
//     call lands, so ProcessSignal sends the entry, then the TP/SL order, and
//     logs a loud error if the second call fails. the returned tpslId is
//     cached so UpdateStopLoss can amend it (POST /api/v1/trade/amend-tpsl)
//     instead of cancelling and re-placing.
//
//     note: order-tpsl is the standalone TP/SL endpoint. order-algo is a
//     different endpoint for trigger / trailing_stop orders and is NOT what
//     this adapter uses for the hard stop.
//
// DEMO-ONLY IN THIS BUILD. the constructor throws if spec_.env != Env::Demo.
// the guard exists so a config that says "mainnet" aborts at startup instead
// of sending a real order through an adapter that has only been exercised
// against the demo book. remove the guard in blofin_broker.cpp when mainnet is
// proven end-to-end.
class BloFinBroker : public IBroker {
public:
    // the spec carries venue, env, category, symbol and every host. there is
    // no default here on purpose: a venue cannot be selected by omission.
    //
    // public_rest_base is the DATA world's REST base, used only as a fallback
    // for the two public market-data calls startup needs (server time and
    // instruments-info). empty means "no fallback": the exec host is the only
    // host.
    BloFinBroker(const std::string& api_key,
                 const std::string& api_secret,
                 const std::string& api_passphrase,
                 mokuren::venue::Spec spec,
                 std::string public_rest_base = "");
    ~BloFinBroker() override;

    // test-only tag: opts out of EstablishStartupTruth() and the private
    // websocket. signing and frame parsing are pure logic and need no socket.
    struct OfflineForTests {};
    BloFinBroker(const std::string& api_key,
                 const std::string& api_secret,
                 const std::string& api_passphrase,
                 mokuren::venue::Spec spec,
                 OfflineForTests);

    void Stop();

    BloFinBroker(const BloFinBroker&) = delete;
    BloFinBroker& operator=(const BloFinBroker&) = delete;

    // ---- IBroker: cached state getters ----
    double GetBalance() const override;
    double GetEquity() const override;
    bool HasOpenPosition() const override;
    TradeSignal GetCurrentPosition() const override;
    void UpdateStopLoss(double new_sl) override;

    // ---- IBroker: orchestration hooks ----
    void ProcessSignal(TradeSignal signal, double fill_price, int64_t current_time) override;
    void CheckRisk(double current_price, int64_t current_time) override;

    void ClosePartial(double fraction, double current_price, int64_t current_time,
                      const std::string& reason) override;

    double GetNetProfit() const override;
    std::vector<TradeRecord> GetTradeHistory() const override;
    double GetLastClosedSize() const override;
    int64_t GetLastTradeTime() const override;
    size_t GetTradeCount() const override;

    // ---- Testing Hooks (Public for Catch2) ----
    //
    // prehash = path_with_query + method_upper + timestamp_ms + nonce + body
    // sig     = base64( hex_string_bytes( HMAC-SHA256(secret, prehash) ) )
    //
    // The chain is the one that will silently produce "signature verification
    // failed" if done the natural way: the digest is hex-encoded to an ASCII
    // string FIRST, and that ASCII string -- not the raw digest, not the hex
    // decoded back to bytes -- is what gets base64'd. The docs call this out
    // explicitly ("not hex2bytes, but string2bytes").
    //
    // `path_with_query` must already include the "?query" for GET requests.
    // `method_upper` must be "GET" or "POST" (uppercase, no whitespace).
    // `timestamp_ms` is the same value sent in ACCESS-TIMESTAMP.
    // `nonce` is the same value sent in ACCESS-NONCE.
    // `body` is the exact JSON string sent as the request body, or "" for GET.
    std::string GenerateSignature(const std::string& path_with_query,
                                  const std::string& method_upper,
                                  const std::string& timestamp_ms,
                                  const std::string& nonce,
                                  const std::string& body);

    // Fresh, unique-per-request nonce. hex(nanoseconds) + "-" + hex(counter).
    // The ns component makes it unique across restarts; the counter makes it
    // unique within a burst on the same nanosecond. Exposed so tests can
    // assert the shape and the no-duplicates property.
    //
    // NOTE: the WS login doc says nonce MAY equal timestamp (their own example
    // does exactly that), so AuthenticatePrivateStream reuses one value for
    // both fields rather than calling this twice. This helper is for the REST
    // path, where the nonce is a per-request header and must be unique.
    static std::string MakeNonce();

    // Mirrors an exchange push frame into the cached state.
    void HandlePrivateMessage(const std::string& payload);

private:
    void EstablishStartupTruth();
    void SyncClock();
    bool FetchInstrumentRules();
    void ReconcileAccount();
    // refuses to start unless the account is net_mode (one-way) / cross. the
    // order payloads below hardcode positionSide="net" and marginMode="cross",
    // and a wrong mode silently turns reduceOnly into a no-op -- so a mismatch
    // is a startup error, not a warning.
    void VerifyAccountMode();

    int64_t NowSignedMillis() const;

    // returns the exchange code (0 = accepted), or -1 on transport/HTTP
    // failure. callers that submit an entry need the distinction.
    //
    // out, when non-null, receives the parsed reply body so the caller can
    // inspect the per-item `data.code`: the top-level code only means
    // "accepted for processing", and an entry that was accepted but rejected
    // at the per-order level must not be treated as filled.
    int SendRestRequest(const std::string& method, const std::string& endpoint,
                        const std::string& payload,
                        nlohmann::json* out = nullptr);

    bool SendRestGet(const std::string& path, const std::string& query,
                     nlohmann::json& out);
    bool SendRestGetTo(const std::string& base, const std::string& path,
                       const std::string& query, nlohmann::json& out);
    bool SendMarketGet(const std::string& path, const std::string& query,
                       nlohmann::json& out);

    bool PendingEntryLocked() const;

    // re-reads the live TP/SL order id from the venue (GET /api/v1/trade/orders-tpsl)
    // and caches it in current_tpsl_id_. returns true when an id is cached.
    // called after a successful order-tpsl placement (the response may omit
    // the id on some builds) and before an amend, so UpdateStopLoss never
    // amends a stale id. takes state_mutex_ internally; callers must not hold
    // it.
    bool RefreshTpslId();

    void ConnectPrivateStream();
    void AuthenticatePrivateStream();
    void SubscribePrivateTopics();
    void HeartbeatLoop();

    // ---- credentials / endpoint routing ----
    std::string api_key_;
    std::string api_secret_;
    std::string api_passphrase_;
    mokuren::venue::Spec spec_;
    std::string rest_base_url_;
    std::string market_rest_base_;
    std::string private_ws_url_;

    // the venue's wire symbol: "BTCUSDT" in the config, "BTC-USDT" on the
    // wire. the translation lives here, not in the config, so a config never
    // has to know which exchange it is talking to -- same reasoning as
    // InstTypeFor. computed once at construction; the config symbol is
    // immutable for the life of the broker.
    std::string wire_symbol_;

    // contract size in base units (BTC per contract). BloFin's BTC-USDT swap
    // is 0.001 BTC/contract. signal.volume is expressed in BASE units (BTC),
    // so the order path divides by this before formatting `size`. populated
    // by FetchInstrumentRules from /api/v1/market/instruments; a zero here
    // means rules were never fetched and any order path must refuse to send.
    double contract_value_ = 0.0;

    // id of the live standalone TP/SL order, as returned by order-tpsl.
    // empty means "no stop on the book right now" -- either none was placed
    // yet, or the last one was cancelled. UpdateStopLoss amends this id
    // rather than cancelling and re-placing, which keeps the stop on the
    // matching engine across a move. guarded by state_mutex_.
    std::string current_tpsl_id_;

    // rate-limit debounce for UpdateStopLoss. blofin allows 30 trading
    // requests / 10 s / UserId, which is the first venue limit that actually
    // bites the entry + SL-move + partial-close chain. a trailed position
    // that moves its stop on every tick would burn the budget in under a
    // second and then start rejecting entries. the debounce drops a move
    // that arrives inside the window rather than queueing it: a stale stop
    // is recoverable (the next tick re-arms it), a rejected entry is not.
    //
    // 250 ms = 4/s, which is ABOVE the sustained 3/s budget, not below it:
    // a position that trails on every tick will still exhaust 30/10 s and
    // start getting rejections. the debounce bounds the SL-move path to 4/s
    // so a single tick cannot fire two moves; it does not make the path
    // rate-safe on its own. the entry + tpsl pair is 2 requests; a
    // ClosePartial immediately after is a 3rd, and those are not debounced.
    static constexpr int64_t kMinSlMoveIntervalMs = 250;
    int64_t last_sl_move_ms_ = 0;

    struct InstrumentRules {
        double tick_size = 0.0;
        double qty_step = 0.0;
        double min_qty = 0.0;
        double min_notional = 0.0;
        double max_leverage = 0.0;
        // base units per contract. mirrored into contract_value_ so the order
        // path can read it without taking state_mutex_ (rules_ is written
        // once at startup, before the ws thread starts).
        double contract_value = 0.0;
        bool valid = false;
    };
    InstrumentRules rules_;

    // server_time_ms - local_time_ms, sampled once at startup from the `ts`
    // field of /api/v1/market/tickers. NowSignedMillis() adds this to the
    // local clock so ACCESS-TIMESTAMP is the venue's time, not ours. a
    // non-zero offset here is the whole point of SyncClock: BloFin rejects
    // any signed request whose timestamp is more than 60 s off, and a
    // container with a drifted clock fails every call with error 152405
    // until someone notices.
    int64_t clock_offset_ms_ = 0;
    uint64_t order_seq_ = 0;

    // nonce counter. atomic because MakeNonce is static and may be called
    // from more than one thread in tests.
    static std::atomic<uint64_t> nonce_counter_;

    ix::WebSocket private_ws_;
    std::atomic<bool> ws_connected_{false};

    std::thread heartbeat_thread_;
    std::atomic<bool> keep_alive_{false};

    mutable std::mutex state_mutex_;

    double current_balance_ = 0.0;
    double position_size_ = 0.0;
    // direction of the open position. kept next to position_size_ because the
    // two are only meaningful together: a non-zero size with NONE direction is
    // the state ClosePartial refuses to reduce, and the state the venue tells
    // us about in `positions` (signed size) rather than in a separate field.
    SignalDirection position_dir_ = SignalDirection::NONE;
    double net_profit_ = 0.0;
    double last_closed_size_ = 0.0;
    TradeSignal current_position_;
    std::vector<TradeRecord> trade_history_;

    mutable bool pending_entry_ = false;
    mutable int64_t pending_since_ms_ = 0;

    double current_sl_ = 0.0;
    double current_tp_ = 0.0;
    double entry_price_ = 0.0;
};
