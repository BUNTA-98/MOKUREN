#pragma once
#include "aggregator.hpp"
#include "filter.hpp"
#include "market_context.hpp"
#include "trigger.hpp"
#include <algorithm>
#include <ctime>
#include <vector>

// base strategy interface
class IStrategy {
public:
  virtual ~IStrategy() = default;
  virtual TradeSignal OnCandleClose(const MarketContext &context) = 0;
  virtual TradeSignal OnTickUpdate(const MarketContext &context) {
    return TradeSignal{};
  }
};

// standard pipeline: evaluates trigger -> runs safety filters -> returns signal
class PipelineStrategy : public IStrategy {
private:
  ITrigger *trigger_;
  std::vector<IFilter *> filters_;

public:
  PipelineStrategy(ITrigger *trigger) : trigger_(trigger) {}

  void AddFilter(IFilter *filter) { filters_.push_back(filter); }

  // evaluate standard indicator triggers on candle close
  TradeSignal OnCandleClose(const MarketContext &context) override {
    if (!trigger_) return TradeSignal{};

    // check if trigger found a setup
    TradeSignal signal = trigger_->EvaluateCandle(context);
    if (signal.direction == SignalDirection::NONE) return TradeSignal{};

    // all filters must allow the trade (early exit saves cpu)
    for (auto *filter : filters_) {
      if (!filter->AllowTrade(context, signal)) {
        return TradeSignal{}; 
      }
    }

    return signal;
  }

  // evaluate hft orderbook triggers on every l2 update
  TradeSignal OnTickUpdate(const MarketContext &context) override {
    if (!trigger_) return TradeSignal{};

    TradeSignal signal = trigger_->EvaluateTick(context);
    if (signal.direction == SignalDirection::NONE) return TradeSignal{};

    for (auto *filter : filters_) {
      if (!filter->AllowTrade(context, signal)) {
        return TradeSignal{};
      }
    }
    return signal;
  }
};