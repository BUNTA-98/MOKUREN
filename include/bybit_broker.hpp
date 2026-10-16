#pragma once

#include "ibroker.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>

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
    BybitBroker(const std::string& api_key,
                const std::string& api_secret,
                bool is_testnet = true);
    ~BybitBroker() override;

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
    // ---- REST plumbing ----
    void SendRestRequest(const std::string& method, const std::string& endpoint,
                         const std::string& payload);

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
    bool is_testnet_ = true;
    std::string rest_base_url_;
    std::string private_ws_url_;

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

    // ---- stop management state (CheckRisk -> UpdateStopLoss) ----
    double current_sl_ = 0.0;
    double current_tp_ = 0.0;
    double entry_price_ = 0.0;
    double highest_seen_price_ = 0.0;
    double lowest_seen_price_ = 0.0;
};
