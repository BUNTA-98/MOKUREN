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

// Live execution adapter for the Bybit V5 API (linear perpetuals).
//
// The IBroker getters are called by the orchestrator on the hot path, so they
// must be cheap: none of them perform a blocking REST round-trip. They are
// served from cached state that is kept current by a private websocket
// subscription to the "position" and "execution" channels.
class BybitBroker : public IBroker {
public:
    // the spec carries env, category, symbol and every host. there is no
    // default here on purpose: a venue cannot be selected by omission.
    BybitBroker(const std::string& api_key,
                const std::string& api_secret,
                mokuren::venue::Spec spec);
    ~BybitBroker() override;

    // test-only tag: opts out of EstablishStartupTruth() and the private
    // websocket. the production constructor must reach the venue (clock,
    // instrument rules, reconciliation) before it may trade, which makes it
    // unusable in a unit test; signing and frame parsing are pure logic and
    // need no socket.
    struct OfflineForTests {};
    BybitBroker(const std::string& api_key,
                const std::string& api_secret,
                mokuren::venue::Spec spec,
                OfflineForTests);

    // stop the heartbeat and the private websocket. idempotent; also invoked
    // by the destructor. call this before the process exits so the socket is
    // torn down deterministically rather than during static destruction.
    void Stop();

    BybitBroker(const BybitBroker&) = delete;
    BybitBroker& operator=(const BybitBroker&) = delete;

    // ---- IBroker: cached state getters ----
    double GetBalance() const override;
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
    // HMAC-SHA256 over the Bybit signing string. A non-empty `timestamp` is
    // prefixed to `payload`; otherwise `payload` is signed verbatim.
    std::string GenerateSignature(const std::string& payload, const std::string& timestamp);
    // Mirrors an exchange push frame into the cached state. Exposed so tests
    // can drive position/execution frames without a live websocket.
    void HandlePrivateMessage(const std::string& payload);

private:
    // ---- live-exchange truth, fetched once at construction ----
    //
    // all three run before the first signal and before any order can be sent.
    // they turn "a default we guessed" into "what the venue actually says",
    // which is the only defensible basis for sizing and for HasOpenPosition().
    // clock/startup facts are established once, before the websocket thread
    // and the engine thread exist, so the fields they fill need no lock.
    void EstablishStartupTruth();
    void SyncClock();            // server-time offset; throws if unusable
    bool FetchInstrumentRules(); // tick/qtyStep/minQty/minNotional/maxLeverage
    void ReconcileAccount();     // adopt an existing position, do not re-enter

    // local time plus the measured server offset. every signature is stamped
    // with this: a few seconds of drift makes valid auth look like a bad key.
    // defined in the .cpp because NowMillis() is a file-local helper there;
    // an inline body here could not see it.
    int64_t NowSignedMillis() const;

    // ---- REST plumbing ----
    // returns the exchange retCode (0 = accepted), or -1 on transport/HTTP
    // failure. callers that submit an entry need the distinction.
    int SendRestRequest(const std::string& method, const std::string& endpoint,
                        const std::string& payload);

    // signed GET. separate from SendRestRequest because a GET signs its query
    // string and carries no body.
    bool SendRestGet(const std::string& path, const std::string& query,
                     nlohmann::json& out);

    // true while a submitted entry awaits its exchange echo. expires so a lost
    // push cannot leave the local guard stuck forever.
    bool PendingEntryLocked() const;

    // ---- private websocket plumbing ----
    void ConnectPrivateStream();
    void AuthenticatePrivateStream();
    void SubscribePrivateTopics();
    // keeps the private socket alive: bybit v5 drops a connection that stays
    // silent for ~30s, and no push arrives while no position is open.
    void HeartbeatLoop();

    // ---- credentials / endpoint routing ----
    std::string api_key_;
    std::string api_secret_;
    mokuren::venue::Spec spec_;
    std::string rest_base_url_;
    std::string private_ws_url_;

    // what this symbol actually accepts. filled by FetchInstrumentRules()
    // before any thread starts; reading it from the engine thread afterwards
    // is safe because it is written exactly once, at construction.
    struct InstrumentRules {
        double tick_size = 0.0;
        double qty_step = 0.0;    // order quantity granularity
        double min_qty = 0.0;     // minOrderQty
        double min_notional = 0.0; // minNotionalValue (0 = no floor published)
        double max_leverage = 0.0;
        bool valid = false;
    };
    InstrumentRules rules_;

    // measured server-minus-local offset. signing against local time is the
    // most common cause of a "bad signature" that is really a clock problem.
    int64_t clock_offset_ms_ = 0;

    // monotonic tag for orderLinkId, so a retried or duplicated submit can be
    // told apart from a new intent. written on the engine thread only.
    uint64_t order_seq_ = 0;

    // listens to the private channel for execution + position updates
    ix::WebSocket private_ws_;
    std::atomic<bool> ws_connected_{false};

    // bybit v5 drops a private socket that stays silent for ~30s, so a
    // heartbeat thread pings it and the socket itself reconnects. keep_alive_
    // gates the thread and is cleared by Stop(). declared after private_ws_ so
    // the thread is joined before the socket is destroyed.
    std::thread heartbeat_thread_;
    std::atomic<bool> keep_alive_{false};

    // Guards every cached field below: written by the websocket thread,
    // read by the engine thread through the const IBroker getters.
    mutable std::mutex state_mutex_;

    // ---- cached broker state (written by the ws thread, read by the engine) ----
    // Guarded by state_mutex_: HandlePrivateMessage writes it from the
    // websocket thread, the const getters read it from the engine thread.
    double current_balance_ = 0.0;
    double position_size_ = 0.0;
    double net_profit_ = 0.0;
    double last_closed_size_ = 0.0;
    TradeSignal current_position_;
    std::vector<TradeRecord> trade_history_;

    // set between a submitted entry and its execution/reject echo. the getters
    // are const and may expire a stale flag while serving a read, hence
    // mutable.
    mutable bool pending_entry_ = false;
    mutable int64_t pending_since_ms_ = 0;

    // ---- stop management state (CheckRisk -> UpdateStopLoss) ----
    double current_sl_ = 0.0;
    double current_tp_ = 0.0;
    double entry_price_ = 0.0;
    double highest_seen_price_ = 0.0;
    double lowest_seen_price_ = 0.0;
};
