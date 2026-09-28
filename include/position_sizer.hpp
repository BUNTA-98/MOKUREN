#pragma once
#include "ibroker.hpp"
#include "papertrader.hpp"
#include "strategy.hpp"
#include <cmath>

class PositionSizer {
private:
  IBroker* broker_;
  double risk_per_trade_pct_;
  double min_sl_distance_;

public:
  // risk_pct: 0.01 bedeutet 1% vom konto
  PositionSizer(IBroker* b, double risk_pct,
                double min_sl_dist = 50)
      : broker_(b), risk_per_trade_pct_(risk_pct),
        min_sl_distance_(min_sl_dist) {}

  TradeSignal CalculateSize(TradeSignal signal) {
    if (signal.direction == SignalDirection::NONE) {
      return signal;
    }

    // schutz: ohne stop-loss keine berechnung möglich
    if (signal.stop_loss == 0.0 || signal.entry_price == 0.0) {
      return TradeSignal{};
    }

    double risk_amount_usd = broker_->GetBalance() * risk_per_trade_pct_;
    double risk_per_coin = std::abs(signal.entry_price - signal.stop_loss);

    if (risk_per_coin < min_sl_distance_) {
      risk_per_coin = min_sl_distance_;

      // ticket anpassen, damit paper trader den neuen sl nutzt
      if (signal.direction == SignalDirection::BUY) {
        signal.stop_loss = signal.entry_price - min_sl_distance_;
      } else {
        signal.stop_loss = signal.entry_price + min_sl_distance_;
      }
    }

    if (risk_per_coin <= 0.0)
      return TradeSignal{};

    // volumen berechnen (fällt jetzt viel kleiner aus bei aufgeweitetem sl!)
    signal.volume = risk_amount_usd / risk_per_coin;

    return signal;
  }
};
