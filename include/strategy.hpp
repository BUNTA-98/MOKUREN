#pragma once
#include "aggregator.hpp"
#include "filter.hpp"
#include "live_debug.hpp"
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
    LIVE_DBG("strategy: OnCandleClose ENTER live_bar_ts={} close={} vol={} history_size={}",
             context.live_bar.timestamp_start, context.live_bar.close,
             context.live_bar.total_volume, context.history.size());

    if (!trigger_) {
      LIVE_DBG("strategy: OnCandleClose ABORT - trigger_ is null");
      return TradeSignal{};
    }

    // check if trigger found a setup
    TradeSignal signal = trigger_->EvaluateCandle(context);
    LIVE_DBG("strategy: OnCandleClose trigger result direction={}",
             static_cast<int>(signal.direction));
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
    LIVE_DBG("strategy: OnTickUpdate ENTER live_bar_ts={} close={}",
             context.live_bar.timestamp_start, context.live_bar.close);

    if (!trigger_) {
      LIVE_DBG("strategy: OnTickUpdate ABORT - trigger_ is null");
      return TradeSignal{};
    }

    TradeSignal signal = trigger_->EvaluateTick(context);
    LIVE_DBG("strategy: OnTickUpdate trigger result direction={}",
             static_cast<int>(signal.direction));
    if (signal.direction == SignalDirection::NONE) return TradeSignal{};

    for (auto *filter : filters_) {
      if (!filter->AllowTrade(context, signal)) {
        return TradeSignal{};
      }
    }
    return signal;
  }
};
