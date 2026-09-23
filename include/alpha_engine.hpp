#pragma once
#include "market_context.hpp"
#include "strategy.hpp"

class AlphaEngine {
private:
    IStrategy& strategy_;

public:
    AlphaEngine(IStrategy& strategy) : strategy_(strategy) {}

  // data in - signal out
    eSignal Evaluate(const MarketContext& context, bool candle_finished) {
        if (candle_finished) {
            return strategy_.OnCandleClose(context);
        } else {
            return strategy_.OnTickUpdate(context);
        }
    }
};
