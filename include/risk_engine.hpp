#pragma once
#include "ibroker.hpp"
#include "strategy.hpp"
#include <vector>

class IRiskModule {
public:
  virtual ~IRiskModule() = default;
  virtual bool CheckRisk(const TradeSignal &signal, int64_t current_time) = 0;
};

class SinglePositionLock : public IRiskModule {
private:
  IBroker* broker_;

public:
  SinglePositionLock(IBroker* broker) : broker_(broker) {}

  bool CheckRisk(const TradeSignal &signal, int64_t current_time) override {
    if (broker_->HasOpenPosition()) return false;
    return true;
  }
};

class MaxDrawdownLock : public IRiskModule {
private:
  IBroker* broker_;
  double max_loss_usd_;

public:
  MaxDrawdownLock(IBroker* broker, double max_loss)
      : broker_(broker), max_loss_usd_(max_loss) {}

  bool CheckRisk(const TradeSignal &signal, int64_t current_time) override {
    if (broker_->GetNetProfit() <= -max_loss_usd_) return false;
    return true;
  }
};

class MaxLeverageLock : public IRiskModule {
private:
  IBroker* broker_;
  double max_leverage_;

public:
  MaxLeverageLock(IBroker* broker, double max_leverage)
      : broker_(broker), max_leverage_(max_leverage) {}

  bool CheckRisk(const TradeSignal &signal, int64_t current_time) override {
    if (signal.entry_price == 0.0 || signal.volume == 0.0) return false;
    double notional_value = signal.volume * signal.entry_price;
    double current_leverage = notional_value / broker_->GetBalance();
    if (current_leverage > max_leverage_) return false;
    return true;
  }
};

class AntiRevengeLock : public IRiskModule {
private:
  IBroker* broker_;
  int64_t cooldown_ms_;

public:
  AntiRevengeLock(IBroker* broker, int64_t cooldown_ms = 1800000) 
      : broker_(broker), cooldown_ms_(cooldown_ms) {}

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

class RiskManager {
private:
  std::vector<IRiskModule *> modules_;

public:
  void AddModule(IRiskModule *module) { modules_.push_back(module); }

  TradeSignal Evaluate(TradeSignal signal, int64_t current_time) {
    if (signal.direction == SignalDirection::NONE) return signal;

    for (auto *module : modules_) {
      if (!module->CheckRisk(signal, current_time)) {
        return TradeSignal{}; 
      }
    }
    return signal; 
  }
};