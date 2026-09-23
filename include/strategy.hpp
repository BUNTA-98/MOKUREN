#pragma once
#include "aggregator.hpp"
#include "market_context.hpp" // Neu: Enthält MarketContext, EngineConfig & IStrategy
#include <algorithm>
#include <iostream>
#include <vector>

class IStrategy {
public:
  virtual ~IStrategy() = default;

  // Für Signale nach Kerzenschluss
  virtual eSignal OnCandleClose(const MarketContext &context) = 0;

  // Für Live-Tick-Signale (z. B. Imbalances & Absorption in der Live-Kerze)
  virtual eSignal OnTickUpdate(const MarketContext &context) {
    return eSignal::NONE; // Standardmäßig ignoriert, wenn nicht überschrieben
  }
};

class ImbalanceStrategy : public IStrategy {
private:
  double ratio_threshold;
  int min_stacked_count;

public:
  ImbalanceStrategy(double ratio, int min_stacked)
      : ratio_threshold(ratio), min_stacked_count(min_stacked) {}

  eSignal OnCandleClose(const MarketContext &context) override {
    // Greift jetzt über den Context auf die Historie zu
    if (context.history.empty())
      return eSignal::NONE;

    // Wir analysieren die aktuellste, frisch geschlossene Kerze aus der
    // Historie
    const Bar &bar = context.history.back();

    int current_buy_stacked = 0;
    int max_buy_stacked = 0;
    int current_sell_stacked = 0;
    int max_sell_stacked = 0;

    for (int i = 1; i < MAX_GRID_LEVELS; i++) {
      const auto &upper = bar.vap_grid[i];
      const auto &lower = bar.vap_grid[i - 1];

      // 1. Überspringe leere Level im Grid
      if (upper.price == 0.0 || lower.price == 0.0) {
        current_buy_stacked = 0;
        current_sell_stacked = 0;
        continue;
      }

      // 2. Prüfe, ob es wirklich direkt benachbarte Preisstufen sind
      if (std::abs((upper.price - lower.price) - bar.tick_size) > 1e-5) {
        current_buy_stacked = 0;
        current_sell_stacked = 0;
        continue;
      }

      if (lower.bid_volume > 0.0 &&
          upper.ask_volume >= lower.bid_volume * ratio_threshold) {
        current_buy_stacked++;
        max_buy_stacked = std::max(max_buy_stacked, current_buy_stacked);
      } else {
        current_buy_stacked = 0;
      }

      if (upper.ask_volume > 0.0 &&
          lower.bid_volume >= upper.ask_volume * ratio_threshold) {
        current_sell_stacked++;
        max_sell_stacked = std::max(max_sell_stacked, current_sell_stacked);
      } else {
        current_sell_stacked = 0;
      }
    }

    // Debug-Druck bei Kerzenschluss
    /*
      std::cout << "[STRATEGY DEBUG] Active Levels: " << bar.active_levels
              << " | Buy Stacked: " << max_buy_stacked
              << " | Sell Stacked: " << max_sell_stacked << std::endl;
    */

    if (max_buy_stacked >= min_stacked_count &&
        max_buy_stacked > max_sell_stacked) {
      return eSignal::BUY;
    } else if (max_sell_stacked >= min_stacked_count &&
               max_sell_stacked > max_buy_stacked) {
      return eSignal::SELL;
    }

    return eSignal::NONE;
  }

  // Optional: Falls du später Live-Imbalances in der aktiven Kerze prüfen
  // willst
  eSignal OnTickUpdate(const MarketContext &context) override {
    // Hier könntest du genau dieselbe Schleife auf context.live_bar anwenden!
    return eSignal::NONE;
  }
};
