#pragma once
#include "papertrader.hpp"
#include "strategy.hpp"
#include <vector>

// basis interface
class IRiskModule {
public:
  virtual ~IRiskModule() = default;
  // NEU: current_time übergeben, damit zeitbasierte Locks funktionieren
  virtual bool CheckRisk(const TradeSignal &signal, int64_t current_time) = 0;
};

// wächter 1: max 1 offene position
class SinglePositionLock : public IRiskModule {
private:
  const PaperTrader &trader_;

public:
  SinglePositionLock(const PaperTrader &trader) : trader_(trader) {}

  bool CheckRisk(const TradeSignal &signal, int64_t current_time) override {
    if (trader_.GetPositionSize() > 0.0) return false;
    return true;
  }
};

// wächter 2: hard drawdown limit
class MaxDrawdownLock : public IRiskModule {
private:
  const PaperTrader &trader_;
  double max_loss_usd_;

public:
  MaxDrawdownLock(const PaperTrader &trader, double max_loss)
      : trader_(trader), max_loss_usd_(max_loss) {}

  bool CheckRisk(const TradeSignal &signal, int64_t current_time) override {
    if (trader_.GetNetProfit() <= -max_loss_usd_) return false;
    return true;
  }
};

// wächter 3: max hebel limit
class MaxLeverageLock : public IRiskModule {
private:
  const PaperTrader &trader_;
  double max_leverage_;

public:
  MaxLeverageLock(const PaperTrader &trader, double max_leverage)
      : trader_(trader), max_leverage_(max_leverage) {}

  bool CheckRisk(const TradeSignal &signal, int64_t current_time) override {
    if (signal.entry_price == 0.0 || signal.volume == 0.0) return false;
    double notional_value = signal.volume * signal.entry_price;
    double current_leverage = notional_value / trader_.GetBalance();
    if (current_leverage > max_leverage_) return false;
    return true;
  }
};

// NEU: wächter 4: anti-revenge cooldown
class AntiRevengeLock : public IRiskModule {
private:
  const PaperTrader &trader_;
  int64_t cooldown_ms_;

public:
  // Standard-Cooldown: 30 Minuten (1.800.000 ms)
  AntiRevengeLock(const PaperTrader &trader, int64_t cooldown_ms = 1800000) 
      : trader_(trader), cooldown_ms_(cooldown_ms) {}

  bool CheckRisk(const TradeSignal &signal, int64_t current_time) override {
    const auto& history = trader_.GetTradeHistory();
    if (history.empty()) return true;

    int64_t last_exit = history.back().exit_time;
    if (current_time - last_exit < cooldown_ms_) {
      return false; // Cooldown noch aktiv -> Blockieren
    }
    return true;
  }
};

// haupt firewall
class RiskManager {
private:
  std::vector<IRiskModule *> modules_;

public:
  void AddModule(IRiskModule *module) { modules_.push_back(module); }

  TradeSignal Evaluate(TradeSignal signal, int64_t current_time) {
    if (signal.direction == SignalDirection::NONE) {
      return signal;
    }

    // alle wächter prüfen
    for (auto *module : modules_) {
      if (!module->CheckRisk(signal, current_time)) {
        return TradeSignal{}; // blockiert
      }
    }

    return signal; // freigegeben
  }
};