#pragma once
#include "ibroker.hpp"
#include "logger.hpp"
#include "strategy.hpp"
#include <vector>

// base interface for all risk modules
class IRiskModule {
public:
  virtual ~IRiskModule() = default;
  // returns true if trade is safe to execute
  virtual bool CheckRisk(const TradeSignal &signal, int64_t current_time) = 0;
  // human-readable name used for audit logging
  virtual const char* Name() const = 0;
};

// blocks signals if a trade is already active
class SinglePositionLock : public IRiskModule {
private:
  IBroker* broker_;

public:
  SinglePositionLock(IBroker* broker) : broker_(broker) {}

  const char* Name() const override { return "SinglePositionLock"; }

  bool CheckRisk(const TradeSignal &signal, int64_t current_time) override {
    if (broker_->HasOpenPosition()) return false;
    return true;
  }
};

// blocks trading if daily loss limit is hit
class MaxDrawdownLock : public IRiskModule {
private:
  IBroker* broker_;
  double max_loss_usd_;

public:
  MaxDrawdownLock(IBroker* broker, double max_loss)
      : broker_(broker), max_loss_usd_(max_loss) {}

  const char* Name() const override { return "MaxDrawdownLock"; }

  bool CheckRisk(const TradeSignal &signal, int64_t current_time) override {
    if (broker_->GetNetProfit() <= -max_loss_usd_) return false;
    return true;
  }
};

// protects margin by limiting total leverage
class MaxLeverageLock : public IRiskModule {
private:
  IBroker* broker_;
  double max_leverage_;

public:
  MaxLeverageLock(IBroker* broker, double max_leverage)
      : broker_(broker), max_leverage_(max_leverage) {}

  const char* Name() const override { return "MaxLeverageLock"; }

  bool CheckRisk(const TradeSignal &signal, int64_t current_time) override {
    if (signal.entry_price == 0.0 || signal.volume == 0.0) return false;
    double notional_value = signal.volume * signal.entry_price;
    double current_leverage = notional_value / broker_->GetBalance();
    if (current_leverage > max_leverage_) return false;
    return true;
  }
};

// blocks trades with excessively wide structural stops
class MaxStopDistanceLock : public IRiskModule {
private:
  double max_stop_pct_; // hard config limit (e.g. 0.02 for 2%)

public:
  MaxStopDistanceLock(double max_stop_pct) : max_stop_pct_(max_stop_pct) {}

  const char* Name() const override { return "MaxStopDistanceLock"; }

  bool CheckRisk(const TradeSignal &signal, int64_t current_time) override {
    // reject if no stop is set (suicide)
    if (signal.entry_price == 0.0 || signal.stop_loss == 0.0) return false;
    
    double stop_distance = std::abs(signal.entry_price - signal.stop_loss);
    double stop_pct = stop_distance / signal.entry_price;
    
    // reject if structural stop exceeds config limit
    if (stop_pct > max_stop_pct_) {
      return false;
    }
    return true;
  }
};

// enforces cooldown after closing a trade to prevent revenge trading
class AntiRevengeLock : public IRiskModule {
private:
  IBroker* broker_;
  int64_t cooldown_ms_;

public:
  AntiRevengeLock(IBroker* broker, int64_t cooldown_ms = 1800000) 
      : broker_(broker), cooldown_ms_(cooldown_ms) {}

  const char* Name() const override { return "AntiRevengeLock"; }

  bool CheckRisk(const TradeSignal &signal, int64_t current_time) override {
    const auto& history = broker_->GetTradeHistory();
    if (history.empty()) return true;

    int64_t last_exit = history.back().exit_time;
    if (current_time - last_exit < cooldown_ms_) {
      return false; 
    }
    return true;
  }
};

// runs signal through all active risk modules
class RiskManager {
private:
  std::vector<IRiskModule *> modules_;

public:
  void AddModule(IRiskModule *module) { modules_.push_back(module); }

  TradeSignal Evaluate(TradeSignal signal, int64_t current_time) {
    if (signal.direction == SignalDirection::NONE) return signal;

    for (auto *module : modules_) {
      if (!module->CheckRisk(signal, current_time)) {
        LOG_TRACE("risk reject: module={} dir={} entry={} sl={} tp={} vol={} t={}",
                  module->Name(),
                  static_cast<int>(signal.direction),
                  signal.entry_price, signal.stop_loss,
                  signal.take_profit, signal.volume, current_time);
        return TradeSignal{}; // signal rejected by firewall
      }
    }
    LOG_TRACE("risk pass: dir={} entry={} sl={} tp={} vol={} t={}",
              static_cast<int>(signal.direction),
              signal.entry_price, signal.stop_loss,
              signal.take_profit, signal.volume, current_time);
    return signal; // all checks passed
  }
};
