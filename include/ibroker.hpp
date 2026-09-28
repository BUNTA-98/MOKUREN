#pragma once
#include "aggregator.hpp"
#include <vector>
#include <string>

// Das Struct zieht aus der papertrader.hpp hierher um!
struct TradeRecord {
  int64_t entry_time;
  int64_t exit_time;
  SignalDirection direction;
  double entry_price;
  double exit_price;
  double net_profit;
  std::string exit_reason;
};

class IBroker {
public:
    virtual ~IBroker() = default;
    
    virtual double GetBalance() const = 0;
    virtual bool HasOpenPosition() const = 0;
    virtual TradeSignal GetCurrentPosition() const = 0;
    virtual void UpdateStopLoss(double new_sl) = 0;
    
    // NEU: Damit die Risk-Module ihre Checks machen können
    virtual double GetNetProfit() const = 0;
    virtual const std::vector<TradeRecord>& GetTradeHistory() const = 0;
};