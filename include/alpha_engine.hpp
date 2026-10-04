#pragma once
#include "market_context.hpp"
#include "strategy.hpp"

class AlphaEngine {
private:
  IStrategy &strategy_;

public:
  AlphaEngine(IStrategy &strategy) : strategy_(strategy) {}

  // route signal evaluation based on tick or candle resolution
  TradeSignal Evaluate(const MarketContext &context, bool candle_finished) {
    if (candle_finished) {
      return strategy_.OnCandleClose(context);
    }
    return strategy_.OnTickUpdate(context);
  }
};