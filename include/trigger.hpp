#pragma once
#include "aggregator.hpp"
#include "market_context.hpp"
#include <algorithm>
#include <ctime>
#include <vector>
#include <cmath>

// base interface for all triggers
class ITrigger {
public:
  virtual ~ITrigger() = default;
  virtual TradeSignal EvaluateCandle(const MarketContext &context) = 0;
  virtual TradeSignal EvaluateTick(const MarketContext &context) {
    return TradeSignal{};
  }
};

// detects suddenly pulled limit orders (spoofing) using the l2 ring buffer
class SpoofHunterTrigger : public ITrigger {
private:
  int lookback_ms_;
  double min_wall_qty_;
  double drop_threshold_; 

public:
  // lookback_ms: how far back to look (e.g. 500ms)
  // min_wall_qty: minimum size of the fake wall (e.g. 30.0 btc)
  // drop_threshold: how much of the wall must disappear (0.9 = 90%)
  SpoofHunterTrigger(int lookback_ms = 500, double min_wall_qty = 30.0, double drop_threshold = 0.9)
      : lookback_ms_(lookback_ms), min_wall_qty_(min_wall_qty), drop_threshold_(drop_threshold) {}

  TradeSignal EvaluateCandle(const MarketContext &context) override {
    return TradeSignal{}; 
  }

  TradeSignal EvaluateTick(const MarketContext& context) override {
    TradeSignal signal;
    
    // get live orderbook state
    const auto& current_l2 = context.latest_l2;
    if (current_l2.timestamp == 0) return signal;

    // search ring buffer for correct snapshot in the past
    const L2Snapshot* past_l2 = nullptr;
    for (size_t i = 0; i < context.l2_history.count; ++i) {
        const auto* snap = context.l2_history.get_history(i);
        
        // stop as soon as we found a snapshot that is old enough
        if (snap && (current_l2.timestamp - snap->timestamp) >= lookback_ms_) {
            past_l2 = snap;
            break;
        }
    }

    // no valid history found yet (buffer filling up)
    if (!past_l2) return signal;

    // check ask spoofing (fake sell wall pulled -> price shoots up -> buy signal)
    if (past_l2->best_ask_qty >= min_wall_qty_) {
        double drop_limit = past_l2->best_ask_qty * (1.0 - drop_threshold_);
        if (current_l2.best_ask_qty <= drop_limit) {
            signal.direction = SignalDirection::BUY;
            return signal;
        }
    }

    // check bid spoofing (fake buy wall pulled -> price drops -> sell signal)
    if (past_l2->best_bid_qty >= min_wall_qty_) {
        double drop_limit = past_l2->best_bid_qty * (1.0 - drop_threshold_);
        if (current_l2.best_bid_qty <= drop_limit) {
            signal.direction = SignalDirection::SELL;
            return signal;
        }
    }

    return signal;
  }
};

// detects stacked footprint imbalances
class StackedImbalanceTrigger : public ITrigger {
private:
  double ratio_threshold;
  int min_stacked_count;

public:
  StackedImbalanceTrigger(double ratio, int min_stacked)
      : ratio_threshold(ratio), min_stacked_count(min_stacked) {}

  TradeSignal EvaluateCandle(const MarketContext &context) override {
    if (context.history.empty()) return TradeSignal{};
    const Bar &bar = context.history.back();

    if (bar.footprint.size() < 2) return TradeSignal{};

    // auto-detect tick size from footprint data
    double real_tick = 999999.0;
    for (size_t i = 1; i < bar.footprint.size(); i++) {
        double diff = bar.footprint[i].price - bar.footprint[i-1].price;
        if (diff > 1e-5 && diff < real_tick) {
            real_tick = diff;
        }
    }
    if (real_tick == 999999.0) real_tick = 1.0; // fallback

    int current_buy_stacked = 0, max_buy_stacked = 0;
    int current_sell_stacked = 0, max_sell_stacked = 0;

    for (size_t i = 1; i < bar.footprint.size(); i++) {
      const auto &upper = bar.footprint[i];
      const auto &lower = bar.footprint[i - 1];

      // dynamic gap check (reset if price jumps)
      if (upper.price == 0.0 || lower.price == 0.0 || 
          (upper.price - lower.price) > (real_tick * 1.5)) {
        current_buy_stacked = 0;
        current_sell_stacked = 0;
        continue;
      }

      // buy imbalance
      bool is_buy_imb = (upper.ask_volume > 0.0001) && 
                        (lower.bid_volume <= 0.0001 || upper.ask_volume >= lower.bid_volume * ratio_threshold);
      
      if (is_buy_imb) {
        current_buy_stacked++;
        max_buy_stacked = std::max(max_buy_stacked, current_buy_stacked);
      } else {
        current_buy_stacked = 0;
      }

      // sell imbalance
      bool is_sell_imb = (upper.bid_volume > 0.0001) && 
                         (lower.ask_volume <= 0.0001 || upper.bid_volume >= lower.ask_volume * ratio_threshold);

      if (is_sell_imb) {
        current_sell_stacked++;
        max_sell_stacked = std::max(max_sell_stacked, current_sell_stacked);
      } else {
        current_sell_stacked = 0;
      }
    }

    if (max_buy_stacked >= min_stacked_count && max_buy_stacked > max_sell_stacked)
      return TradeSignal{SignalDirection::BUY};
      
    if (max_sell_stacked >= min_stacked_count && max_sell_stacked > max_buy_stacked)
      return TradeSignal{SignalDirection::SELL};

    return TradeSignal{};
  }
};

// detects absorption (price moves against heavy aggressive flow)
class DeltaAbsorptionTrigger : public ITrigger {
private:
  double min_delta_threshold_;

public:
  DeltaAbsorptionTrigger(double min_delta) : min_delta_threshold_(min_delta) {}

  TradeSignal EvaluateCandle(const MarketContext &context) override {
    if (context.history.empty()) return TradeSignal{};
    const Bar &bar = context.history.back();

    double total_delta = 0.0;
    
    // calc net delta from footprint
    for (const auto &level : bar.footprint) {
      if (level.price > 0.0) {
        total_delta += (level.ask_volume - level.bid_volume);
      }
    }

    // buy signal: heavy selling pressure but candle closes green
    if (total_delta <= -min_delta_threshold_ && bar.close > bar.open) {
      TradeSignal ticket;
      ticket.direction = SignalDirection::BUY;
      ticket.entry_price = bar.close; 
      ticket.stop_loss = bar.low - (bar.tick_size * 2);

      // basic 2:1 risk reward
      double risk = ticket.entry_price - ticket.stop_loss;
      ticket.take_profit = ticket.entry_price + (risk * 2);

      return ticket;
    }

    // sell signal: heavy buying pressure but candle closes red
    if (total_delta >= min_delta_threshold_ && bar.close < bar.open) {
      return TradeSignal{SignalDirection::SELL};
    }

    return TradeSignal{};
  }
};

// logical or block (first valid signal wins)
class OR_Trigger : public ITrigger {
private:
  std::vector<ITrigger *> triggers_;

public:
  void AddTrigger(ITrigger *trigger) { triggers_.push_back(trigger); }

  TradeSignal EvaluateCandle(const MarketContext &context) override {
    for (auto *t : triggers_) {
      TradeSignal signal = t->EvaluateCandle(context);
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

// logical and block (requires unanimous consensus)
class AND_Trigger : public ITrigger {
private:
  std::vector<ITrigger *> triggers_;

public:
  void AddTrigger(ITrigger *trigger) { triggers_.push_back(trigger); }

  TradeSignal EvaluateCandle(const MarketContext &context) override {
    if (triggers_.empty()) return TradeSignal{};

    // get baseline consensus from first trigger
    TradeSignal consensus = triggers_[0]->EvaluateCandle(context);
    if (consensus.direction == SignalDirection::NONE) return TradeSignal{};

    // verify consensus across remaining triggers
    for (size_t i = 1; i < triggers_.size(); ++i) {
      if (triggers_[i]->EvaluateCandle(context).direction != consensus.direction) {
        return TradeSignal{}; 
      }
    }

    return consensus; 
  }
};