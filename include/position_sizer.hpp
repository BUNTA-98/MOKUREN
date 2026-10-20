#pragma once
#include "ibroker.hpp"
#include "papertrader.hpp"
#include "strategy.hpp"
#include <cmath>
#include <algorithm>

class PositionSizer {
private:
  IBroker* broker_;
  double risk_per_trade_pct_;
  double min_sl_distance_;
  double lot_step_;     // NEU: Exchange Lot-Größen-Raster (z.B. 0.001 für BTC)
  double max_leverage_; // NEU: Maximaler Hebel als Margin-Schutz

public:
  // min_sl_dist is an absolute quote-currency floor, in the same units as
  // risk.min_distance_dollars. it used to default to 15.0 USD, which is noise
  // for BTC and a 6%+ stop for SOL: a caller that did not pass one silently
  // widened every stop. 0 -- the AppConfig default -- means "never widen the
  // trigger's own stop", so this must not drift from that struct again.
  PositionSizer(IBroker* b, double risk_pct, double min_sl_dist = 0.0,
                double lot_step = 0.001, double max_leverage = 20.0)
      : broker_(b), risk_per_trade_pct_(risk_pct),
        min_sl_distance_(min_sl_dist), lot_step_(lot_step), max_leverage_(max_leverage) {}

  TradeSignal CalculateSize(TradeSignal signal) {
    if (signal.direction == SignalDirection::NONE || signal.stop_loss == 0.0 || signal.entry_price == 0.0) {
      return TradeSignal{};
    }

    double balance = broker_->GetBalance();
    double risk_amount_usd = balance * risk_per_trade_pct_;
    double risk_per_coin = std::abs(signal.entry_price - signal.stop_loss);

    // SL Weitung bei zu engem Risk
    if (risk_per_coin < min_sl_distance_) {
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
    // a zero/negative cap means "no cap", not "block every order".
    if (max_leverage_ > 0.0) {
      double max_allowed_volume = (balance * max_leverage_) / signal.entry_price;
      if (raw_volume > max_allowed_volume) {
          raw_volume = max_allowed_volume;
      }
    }

    // --- PRO-UPGRADE 2: LOT STEP ROUNDING ---
    // Börsen akzeptieren keine endlosen Kommastellen
    // only round when a step is configured: dividing by a zero step produced
    // an infinite volume, which is exactly the order a venue must never see.
    double rounded_volume = raw_volume;
    if (lot_step_ > 0.0) {
      rounded_volume = std::floor(raw_volume / lot_step_) * lot_step_;
    }

    if (rounded_volume <= 0.0) return TradeSignal{}; // Zu klein für die Börse

    signal.volume = rounded_volume;
    return signal;
  }
};
