#pragma once
#include "ibroker.hpp"
#include "logger.hpp"
#include "papertrader.hpp"
#include "strategy.hpp"
#include <cmath>
#include <algorithm>

// Physical sanity ceiling on notional / balance, in multiples of equity. This
// is deliberately NOT the policy number: policy lives in MaxLeverageLock, where
// a breach vetoes the signal instead of silently shrinking it. The factory
// rejects any policy above this, so the ceiling can never fire before the lock.
//
// One owner matters because leverage == risk_per_trade_pct / stop_pct: a size
// cap and a leverage veto are the SAME decision. Keeping both, at different
// numbers, let a tight structural stop be truncated to the cap and then
// recorded as if it had risked the configured fraction -- so rows with
// different stop widths were compared as if their risk were equal.
inline constexpr double kVenueMaxLeverage = 125.0;

class PositionSizer {
private:
  IBroker* broker_;
  double risk_per_trade_pct_;
  double min_sl_distance_;
  double lot_step_;        // NEU: Exchange Lot-Größen-Raster (z.B. 0.001 für BTC)
  double margin_ceiling_;  // physical only: see kVenueMaxLeverage

public:
  // min_sl_dist is an absolute quote-currency floor, in the same units as
  // risk.min_distance_dollars. it used to default to 15.0 USD, which is noise
  // for BTC and a 6%+ stop for SOL: a caller that did not pass one silently
  // widened every stop. 0 -- the AppConfig default -- means "never widen the
  // trigger's own stop", so this must not drift from that struct again.
  //
  // NOTE ON TRAILING: this sizer is shared by every path, but only the live
  // venue path models venue trailing (trail_pct > 0). PaperTrader ignores
  // trail_pct entirely and uses the fixed SL from trade_management, so a
  // backtest with trail_r > 0 does NOT measure what live will do. sweep
  // trail_r on demo only; do not trust the backtest to tell you trailing
  // helps.
  // the last argument is a margin SANITY ceiling, not a strategy limit: callers
  // pass kVenueMaxLeverage. the policy number is MaxLeverageLock's, and the
  // factory refuses any policy above this ceiling, so the two can never
  // disagree about which one decided.
  PositionSizer(IBroker* b, double risk_pct, double min_sl_dist = 0.0,
                double lot_step = 0.001,
                double margin_ceiling = kVenueMaxLeverage)
      : broker_(b), risk_per_trade_pct_(risk_pct),
        min_sl_distance_(min_sl_dist), lot_step_(lot_step),
        margin_ceiling_(margin_ceiling) {}

  TradeSignal CalculateSize(TradeSignal signal) {
    if (signal.direction == SignalDirection::NONE || signal.stop_loss == 0.0 || signal.entry_price == 0.0) {
      return TradeSignal{};
    }

    double balance = broker_->GetBalance();
    double risk_amount_usd = balance * risk_per_trade_pct_;
    double risk_per_coin = std::abs(signal.entry_price - signal.stop_loss);

    // SL Weitung bei zu engem Risk
    if (risk_per_coin < min_sl_distance_) {
      // this redefines 1R: the trigger's structural stop is replaced by a flat
      // quote-currency distance. log it, because "the stop was widened to 25
      // usd on a 200 usd symbol" is otherwise indistinguishable from "the
      // strategy found no setups".
      LOG_WARN("position_sizer: widening stop from {} to {} (entry={})",
               risk_per_coin, min_sl_distance_, signal.entry_price);
      risk_per_coin = min_sl_distance_;
      signal.stop_loss = (signal.direction == SignalDirection::BUY) 
                         ? signal.entry_price - min_sl_distance_ 
                         : signal.entry_price + min_sl_distance_;
    }

    if (risk_per_coin <= 0.0) return TradeSignal{};

    // Rohes Volumen berechnen
    double raw_volume = risk_amount_usd / risk_per_coin;

    // --- PRO-UPGRADE 1: LEVERAGE LIMIT (Margin Schutz) ---
    // Wenn der SL extrem eng ist, könnte die Lot-Größe so riesig werden, 
    // dass sie unser Margin-Limit (Balance * Max Hebel) sprengt.
    // a zero/negative ceiling means "no ceiling", not "block every order".
    if (margin_ceiling_ > 0.0) {
      double max_allowed_volume = (balance * margin_ceiling_) / signal.entry_price;
      if (raw_volume > max_allowed_volume) {
          // Unreachable while MaxLeverageLock is configured: the factory
          // rejects any policy above kVenueMaxLeverage, so the veto fires
          // first. If this ever logs, a guardrail is missing -- that is a
          // degraded run, not a normal resize, which is why it is a WARN.
          LOG_WARN("position_sizer: volume {} exceeds the physical margin "
                   "ceiling {}x ({}); MaxLeverageLock should have rejected "
                   "this signal first -- check that the module is configured",
                   raw_volume, margin_ceiling_, max_allowed_volume);
          raw_volume = max_allowed_volume;
      }
    }

    // --- PRO-UPGRADE 2: LOT STEP ROUNDING ---
    // Börsen akzeptieren keine endlosen Kommastellen
    // only round when a step is configured: dividing by a zero step produced
    // an infinite volume, which is exactly the order a venue must never see.
    double rounded_volume = raw_volume;
    if (lot_step_ > 0.0) {
      // binary floating point cannot represent 0.001 exactly, so a volume that
      // is mathematically an exact multiple of the step can land a hair below
      // it and floor to one lot less (3.0 -> 2 lots). nudge by a relative
      // epsilon before truncating; the overshoot is far below one step.
      const double quotient = raw_volume / lot_step_;
      const double nudged =
          quotient + 1e-9 * std::max(1.0, std::abs(quotient));
      rounded_volume = std::floor(nudged) * lot_step_;
    }

    if (rounded_volume <= 0.0) {
      // a signal dropped here is indistinguishable from a trigger that never
      // fired, so say why instead of returning an empty signal into silence.
      LOG_WARN("position_sizer: dropping signal: volume {} rounds to 0 at "
               "lot_step={} (balance={} risk_amount={} risk_per_coin={})",
               raw_volume, lot_step_, balance, risk_amount_usd, risk_per_coin);
      return TradeSignal{}; // Zu klein für die Börse
    }

    signal.volume = rounded_volume;
    return signal;
  }
};
