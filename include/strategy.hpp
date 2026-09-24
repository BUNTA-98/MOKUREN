/*
 * TODO
 * -split trigger strategy und filter in separate files
 */

#pragma once
#include "aggregator.hpp"
#include "filter.hpp"
#include "market_context.hpp"
#include "trigger.hpp"
#include <algorithm>
#include <ctime>
#include <vector>

class IStrategy {
public:
  virtual ~IStrategy() = default;
  virtual TradeSignal OnCandleClose(const MarketContext &context) = 0;
  virtual TradeSignal OnTickUpdate(const MarketContext &context) {
    return TradeSignal{};
  }
};

class PipelineStrategy : public IStrategy {
private:
  ITrigger *trigger_;
  std::vector<IFilter *> filters_;

public:
  PipelineStrategy(ITrigger *trigger) : trigger_(trigger) {}

  void AddFilter(IFilter *filter) { filters_.push_back(filter); }

  TradeSignal OnCandleClose(const MarketContext &context) override {
    if (!trigger_)
      return TradeSignal{};

    // 1. Hat der Trigger überhaupt ein Setup gefunden?
    TradeSignal signal = trigger_->EvaluateCandle(context);
    if (signal.direction == SignalDirection::NONE)
      return TradeSignal{};

    // 2. Wenn ja, müssen alle Filter grünes Licht geben
    for (auto *filter : filters_) {
      if (!filter->AllowTrade(context, signal)) {
        return TradeSignal{}; // Early Exit! (Spart CPU-Zyklen)
      }
    }

    return signal;
  }

  TradeSignal OnTickUpdate(const MarketContext &context) override {
    if (!trigger_)
      return TradeSignal{};

    TradeSignal signal = trigger_->EvaluateTick(context);
    if (signal.direction == SignalDirection::NONE)
      return TradeSignal{};

    for (auto *filter : filters_) {
      if (!filter->AllowTrade(context, signal)) {
        return TradeSignal{};
      }
    }
    return signal;
  }
};
