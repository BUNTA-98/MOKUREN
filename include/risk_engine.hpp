#pragma once
#include "papertrader.hpp"
#include "strategy.hpp"
#include <vector>

// basis interface
class IRiskModule {
public:
  virtual ~IRiskModule() = default;
  virtual bool CheckRisk(const TradeSignal &signal) = 0;
};

// wächter 1: max 1 offene position
class SinglePositionLock : public IRiskModule {
private:
  const PaperTrader &trader_;

public:
  // ref auf trader für live-daten
  SinglePositionLock(const PaperTrader &trader) : trader_(trader) {}

  bool CheckRisk(const TradeSignal &signal) override {
    // blockt ALLES ab, wenn wir schon im trade sind
    if (trader_.GetPositionSize() > 0.0)
      return false;
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

  bool CheckRisk(const TradeSignal &signal) override {
    // blockiere alles, wenn limit gerissen
    if (trader_.GetNetProfit() <= -max_loss_usd_) {
      return false;
    }
    return true;
  }
};

// wächter 3: max hebel limit
class MaxLeverageLock : public IRiskModule {
private:
  const PaperTrader &trader_;
  double max_leverage_;

public:
  // max_leverage: 1.0 = kein hebel (max 10.000$ posi), 2.0 = 2x hebel
  // (max 20.000$)
  MaxLeverageLock(const PaperTrader &trader, double max_leverage)
      : trader_(trader), max_leverage_(max_leverage) {}

  bool CheckRisk(const TradeSignal &signal) override {
    if (signal.entry_price == 0.0 || signal.volume == 0.0)
      return false;

    // gesamtgröße des trades in dollar
    double notional_value = signal.volume * signal.entry_price;
    double current_leverage = notional_value / trader_.GetBalance();

    // blockt ab, wenn der hebel das limit sprengt
    if (current_leverage > max_leverage_) {
      return false;
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

  TradeSignal Evaluate(TradeSignal signal) {
    if (signal.direction == SignalDirection::NONE) {
      return signal;
    }

    // alle wächter prüfen
    for (auto *module : modules_) {
      if (!module->CheckRisk(signal)) {
        return TradeSignal{}; // blockiert
      }
    }

    return signal; // freigegeben
  }
};
