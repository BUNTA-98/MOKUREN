#pragma once

#include "ibroker.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Live execution adapter for the Hyperliquid exchange.
//
// Hyperliquid does not use API keys: orders are EIP-712 signed with a wallet's
// private key and submitted to POST /exchange. Account state (balance, open
// positions, fills) is pushed over the public websocket "webData2" channel for
// that wallet, so the IBroker getters never need a REST round-trip -- they read
// the mutex-guarded cache below.
//
// The websocket is opened from the constructor (mirroring BybitBroker), which
// means construction performs network I/O.
class HyperliquidBroker : public IBroker {
public:
    HyperliquidBroker(
        const std::string& wallet_address,
        const std::string& private_key,
        const std::string& private_ws_url = "wss://api.hyperliquid.xyz/ws",
        const std::string& rest_url = "https://api.hyperliquid.xyz");
    ~HyperliquidBroker() override;

    HyperliquidBroker(const HyperliquidBroker&) = delete;
    HyperliquidBroker& operator=(const HyperliquidBroker&) = delete;

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
    // Mirrors one websocket push into the cached state. Exposed so tests can
    // drive account/fill frames without a live connection.
    void HandlePrivateMessage(const nlohmann::json& payload);
    // EIP-712 order signing hook. Returns the 65-byte signature as hex, or an
    // empty string while libsecp256k1 is not linked yet.
    std::string SignOrderEIP712(const nlohmann::json& order_payload);

private:
    // dials the private stream and wires the message callback.
    void ConnectPrivateStream();
    // sends the webData2 subscription for wallet_address_.
    void SendSubscription();

    // ---- credentials / endpoint routing ----
    std::string wallet_address_;
    std::string private_key_;
    std::string private_ws_url_;
    std::string rest_url_;

    // user-state channel: webData2 pushes balance, positions, orders and fills.
    ix::WebSocket private_ws_;
    std::atomic<bool> ws_connected_{false};

    // Guards every cached field below: written by the websocket thread,
    // read by the engine thread through the const IBroker getters.
    mutable std::mutex state_mutex_;

    // ---- cached account state ----
    double current_balance_ = 0.0;
    double position_size_ = 0.0;
    // net_profit_ = realized_pnl_ + unrealized_pnl_, measured from process
    // start (hyperliquid reports no lifetime baseline for the wallet).
    double realized_pnl_ = 0.0;
    double unrealized_pnl_ = 0.0;
    double net_profit_ = 0.0;
    double last_closed_size_ = 0.0;
    TradeSignal current_position_;
    std::vector<TradeRecord> trade_history_;

    // ---- stop management state (CheckRisk -> UpdateStopLoss) ----
    double current_sl_ = 0.0;
    double current_tp_ = 0.0;
    double entry_price_ = 0.0;
};
