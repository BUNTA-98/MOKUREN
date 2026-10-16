#pragma once
#include "aggregator.hpp"
#include <vector>
#include <string>

// represents a single adjustment of the stop loss
struct StopEvent {
    int64_t timestamp;
    size_t candle_idx;
    double sl_price;
    std::string type;
};

// core trade data protocol logged by the paper trader
struct TradeRecord {
  int64_t entry_time;
  int64_t exit_time;
  SignalDirection direction;
  double entry_price;
  double exit_price;
  double net_profit;
  std::string exit_reason;
  std::vector<StopEvent> stop_events;
};

// broker interface for execution
class IBroker {
public:
    virtual ~IBroker() = default;
    
    virtual double GetBalance() const = 0;
    virtual bool HasOpenPosition() const = 0;
    virtual TradeSignal GetCurrentPosition() const = 0;
    virtual void UpdateStopLoss(double new_sl) = 0;

    // core execution hooks called by the orchestrator
    virtual void ProcessSignal(TradeSignal signal, double fill_price, int64_t current_time) = 0;
    virtual void CheckRisk(double current_price, int64_t current_time) = 0;
    
    // trigger partial exits for scale outs
    virtual void ClosePartial(double fraction, double current_price, int64_t current_time, const std::string& reason) = 0;
    
    // required for risk manager and engine state synchronization
    virtual double GetNetProfit() const = 0;
    // returns a copy of the log. live brokers append to it from their
    // websocket thread while the engine reads, so handing out a reference
    // would leave the caller iterating a vector that is being mutated.
    virtual std::vector<TradeRecord> GetTradeHistory() const = 0;
    // O(1) accessors: the risk firewall reads the last exit time and the log
    // length on every signal and must not copy the whole vector to do it.
    virtual int64_t GetLastTradeTime() const = 0;
    virtual size_t GetTradeCount() const = 0;

    // size of the most recently closed position (0.0 if none yet).
    // used by the live event broadcaster to report closed volume.
    virtual double GetLastClosedSize() const = 0;
};
