#pragma once
#include "aggregator.hpp"
#include "market_context.hpp"
#include <algorithm>
#include <ctime>
#include <vector>

class ITrigger {
public:
  virtual ~ITrigger() = default;
  virtual TradeSignal EvaluateCandle(const MarketContext &context) = 0;
  virtual TradeSignal EvaluateTick(const MarketContext &context) {
    return TradeSignal{};
  }
};

class StackedImbalanceTrigger : public ITrigger {
private:
  double ratio_threshold;
  int min_stacked_count;

public:
  StackedImbalanceTrigger(double ratio, int min_stacked)
      : ratio_threshold(ratio), min_stacked_count(min_stacked) {}

  TradeSignal EvaluateCandle(const MarketContext &context) override {
    if (context.history.empty())
      return TradeSignal{};
    const Bar &bar = context.history.back();

    int current_buy_stacked = 0, max_buy_stacked = 0;
    int current_sell_stacked = 0, max_sell_stacked = 0;

    for (int i = 1; i < MAX_GRID_LEVELS; i++) {
      const auto &upper = bar.vap_grid[i];
      const auto &lower = bar.vap_grid[i - 1];

      if (upper.price == 0.0 || lower.price == 0.0) {
        current_buy_stacked = 0;
        current_sell_stacked = 0;
        continue;
      }
      if (std::abs((upper.price - lower.price) - bar.tick_size) > 1e-5) {
        current_buy_stacked = 0;
        current_sell_stacked = 0;
        continue;
      }

      if (lower.bid_volume > 0.0 &&
          upper.ask_volume >= lower.bid_volume * ratio_threshold) {
        current_buy_stacked++;
        max_buy_stacked = std::max(max_buy_stacked, current_buy_stacked);
      } else
        current_buy_stacked = 0;

      if (upper.ask_volume > 0.0 &&
          lower.bid_volume >= upper.ask_volume * ratio_threshold) {
        current_sell_stacked++;
        max_sell_stacked = std::max(max_sell_stacked, current_sell_stacked);
      } else
        current_sell_stacked = 0;
    }

    if (max_buy_stacked >= min_stacked_count &&
        max_buy_stacked > max_sell_stacked)
      return TradeSignal{SignalDirection::BUY};
    if (max_sell_stacked >= min_stacked_count &&
        max_sell_stacked > max_buy_stacked)
      return TradeSignal{SignalDirection::SELL};

    return TradeSignal{};
  }
};

class DeltaAbsorptionTrigger : public ITrigger {
private:
  double min_delta_threshold_;

public:
  DeltaAbsorptionTrigger(double min_delta) : min_delta_threshold_(min_delta) {}

  TradeSignal EvaluateCandle(const MarketContext &context) override {
    if (context.history.empty())
      return TradeSignal{};
    const Bar &bar = context.history.back();

    double total_delta = 0.0;
    for (int i = 0; i < MAX_GRID_LEVELS; i++) {
      if (bar.vap_grid[i].price > 0.0) {
        // Delta = Aggressive Käufer (Ask) - Aggressive Verkäufer (Bid)
        total_delta +=
            (bar.vap_grid[i].ask_volume - bar.vap_grid[i].bid_volume);
      }
    }

    // 2. Absorption prüfen
    // BUY-Signal: Extremes negatives Delta (Verkaufsdruck), aber Kerze schließt
    // grün (Käufer halten dagegen)
    if (total_delta <= -min_delta_threshold_ && bar.close > bar.open) {
      TradeSignal ticket;
      ticket.direction = SignalDirection::BUY;
      ticket.entry_price = bar.close; // Wir steigen zum Schlusskurs ein
      ticket.stop_loss = bar.low - (bar.tick_size * 2);

      // Simples 2:1 Risk-Reward für den Take Profit
      double risk = ticket.entry_price - ticket.stop_loss;
      ticket.take_profit = ticket.entry_price + (risk * 2);

      return ticket;
    }

    // SELL-Signal: Extremes positives Delta (Kaufdruck), aber Kerze schließt
    // rot
    if (total_delta >= min_delta_threshold_ && bar.close < bar.open) {
      // Du kannst diesen SELL-Block später analog zum BUY-Block ausbauen
      // und hier ebenfalls SL und TP definieren!
      return TradeSignal{SignalDirection::SELL};
    }

    return TradeSignal{};
  }
};

class OR_Trigger : public ITrigger {
private:
  std::vector<ITrigger *> triggers_;

public:
  void AddTrigger(ITrigger *trigger) { triggers_.push_back(trigger); }

  TradeSignal EvaluateCandle(const MarketContext &context) override {
    for (auto *t : triggers_) {
      TradeSignal signal = t->EvaluateCandle(context);
      // Der erste Trigger, der ein Signal (BUY/SELL) findet, gewinnt!
      if (signal.direction != SignalDirection::NONE) {
        return signal;
      }
    }
    return TradeSignal{};
  }

  TradeSignal EvaluateTick(const MarketContext &context) override {
    for (auto *t : triggers_) {
      TradeSignal signal = t->EvaluateTick(context);
      if (signal.direction != SignalDirection::NONE)
        return signal;
    }
    return TradeSignal{};
  }
};

class AND_Trigger : public ITrigger {
private:
  std::vector<ITrigger *> triggers_;

public:
  void AddTrigger(ITrigger *trigger) { triggers_.push_back(trigger); }

  TradeSignal EvaluateCandle(const MarketContext &context) override {
    if (triggers_.empty())
      return TradeSignal{};

    // first trigger
    TradeSignal consensus = triggers_[0]->EvaluateCandle(context);
    if (consensus.direction == SignalDirection::NONE)
      return TradeSignal{};

    // check consensus
    for (size_t i = 1; i < triggers_.size(); ++i) {
      if (triggers_[i]->EvaluateCandle(context).direction !=
          consensus.direction) {
        return TradeSignal{}; // Uneinigkeit! Trade wird abgebrochen.
      }
    }

    return consensus; // Alle sind sich einig
  }
};