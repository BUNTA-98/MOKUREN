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
    
    // trigger partial exits for scale outs
    virtual void ClosePartial(double fraction, double current_price, int64_t current_time, const std::string& reason) = 0;
    
    // required for risk manager and engine state synchronization
    virtual double GetNetProfit() const = 0;
    virtual const std::vector<TradeRecord>& GetTradeHistory() const = 0;
};