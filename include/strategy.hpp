/*
 * TODO
 * -split trigger strategy und filter in separate files
 */


#pragma once
#include "aggregator.hpp"
#include "market_context.hpp"
#include <algorithm>
#include <vector>
#include <ctime>


class IStrategy {
public:
  virtual ~IStrategy() = default;
  virtual eSignal OnCandleClose(const MarketContext &context) = 0;
  virtual eSignal OnTickUpdate(const MarketContext &context) { return eSignal::NONE; }
};

class IFilter {
public:
  virtual ~IFilter() = default;
  virtual bool AllowTrade(const MarketContext &context, eSignal intended_signal) = 0;
};

class ITrigger {
public:
  virtual ~ITrigger() = default;
  virtual eSignal EvaluateCandle(const MarketContext &context) = 0;
  virtual eSignal EvaluateTick(const MarketContext &context) { return eSignal::NONE; }
};



class PipelineStrategy : public IStrategy {
private:
  ITrigger* trigger_;
  std::vector<IFilter*> filters_;

public:
  PipelineStrategy(ITrigger* trigger) : trigger_(trigger) {}

  void AddFilter(IFilter* filter) {
    filters_.push_back(filter);
  }

  eSignal OnCandleClose(const MarketContext &context) override {
    if (!trigger_) return eSignal::NONE;

    // 1. Hat der Trigger überhaupt ein Setup gefunden?
    eSignal signal = trigger_->EvaluateCandle(context);
    if (signal == eSignal::NONE) return eSignal::NONE;

    // 2. Wenn ja, müssen alle Filter grünes Licht geben
    for (auto* filter : filters_) {
      if (!filter->AllowTrade(context, signal)) {
        return eSignal::NONE; // Early Exit! (Spart CPU-Zyklen)
      }
    }

    return signal;
  }

  eSignal OnTickUpdate(const MarketContext &context) override {
    if (!trigger_) return eSignal::NONE;

    eSignal signal = trigger_->EvaluateTick(context);
    if (signal == eSignal::NONE) return eSignal::NONE;

    for (auto* filter : filters_) {
      if (!filter->AllowTrade(context, signal)) {
        return eSignal::NONE;
      }
    }
    return signal;
  }
};


class StackedImbalanceTrigger : public ITrigger {
private:
  double ratio_threshold;
  int min_stacked_count;

public:
  StackedImbalanceTrigger(double ratio, int min_stacked)
      : ratio_threshold(ratio), min_stacked_count(min_stacked) {}

  eSignal EvaluateCandle(const MarketContext &context) override {
    if (context.history.empty()) return eSignal::NONE;
    const Bar &bar = context.history.back();

    int current_buy_stacked = 0, max_buy_stacked = 0;
    int current_sell_stacked = 0, max_sell_stacked = 0;

    for (int i = 1; i < MAX_GRID_LEVELS; i++) {
      const auto &upper = bar.vap_grid[i];
      const auto &lower = bar.vap_grid[i - 1];

      if (upper.price == 0.0 || lower.price == 0.0) {
        current_buy_stacked = 0; current_sell_stacked = 0;
        continue;
      }
      if (std::abs((upper.price - lower.price) - bar.tick_size) > 1e-5) {
        current_buy_stacked = 0; current_sell_stacked = 0;
        continue;
      }

      if (lower.bid_volume > 0.0 && upper.ask_volume >= lower.bid_volume * ratio_threshold) {
        current_buy_stacked++;
        max_buy_stacked = std::max(max_buy_stacked, current_buy_stacked);
      } else current_buy_stacked = 0;

      if (upper.ask_volume > 0.0 && lower.bid_volume >= upper.ask_volume * ratio_threshold) {
        current_sell_stacked++;
        max_sell_stacked = std::max(max_sell_stacked, current_sell_stacked);
      } else current_sell_stacked = 0;
    }

    if (max_buy_stacked >= min_stacked_count && max_buy_stacked > max_sell_stacked) return eSignal::BUY;
    if (max_sell_stacked >= min_stacked_count && max_sell_stacked > max_buy_stacked) return eSignal::SELL;

    return eSignal::NONE;
  }
};

class MinVolumeFilter : public IFilter {
private:
  double min_volume_;
public:
  MinVolumeFilter(double min_volume) : min_volume_(min_volume) {}

  bool AllowTrade(const MarketContext &context, eSignal intended_signal) override {
    if (context.history.empty()) return false;
    // Blockiert den Trade, wenn die letzte Kerze extrem wenig Volumen hatte (Choppiness)
    return context.history.back().total_volume >= min_volume_; 
  }
};


class TimeOfDayFilter : public IFilter {
private:
  int start_hour_, start_min_;
  int end_hour_, end_min_;

public:
  TimeOfDayFilter(int start_h, int start_m, int end_h, int end_m)
      : start_hour_(start_h), start_min_(start_m), 
        end_hour_(end_h), end_min_(end_m) {}

  bool AllowTrade(const MarketContext &context, eSignal intended_signal) override {
    if (context.history.empty()) return false;
    
    // Unix-Timestamp der Kerze in lokale Zeit umwandeln
    std::time_t time_val = context.history.back().timestamp_start / 1000;
    std::tm* time_info = std::localtime(&time_val);
    
    int current_mins_of_day = time_info->tm_hour * 60 + time_info->tm_min;
    int start_mins_of_day = start_hour_ * 60 + start_min_;
    int end_mins_of_day = end_hour_ * 60 + end_min_;

    // Prüfen, ob die aktuelle Zeit im Zeitfenster liegt
    return (current_mins_of_day >= start_mins_of_day && current_mins_of_day <= end_mins_of_day);
  }
};

class POCTrendFilter : public IFilter {
public:
  bool AllowTrade(const MarketContext &context, eSignal intended_signal) override {
    if (context.history.empty()) return false;
    
    const Bar &last_bar = context.history.back();
    
    // Wenn kein POC berechnet wurde (sollte nicht passieren), erlaube den Trade
    if (last_bar.poc_price == 0.0) return true;

    if (intended_signal == eSignal::BUY) {
      // Nur kaufen, wenn wir über dem stärksten Volumenknoten geschlossen haben
      return last_bar.close > last_bar.poc_price;
    } 
    else if (intended_signal == eSignal::SELL) {
      // Nur shorten, wenn wir unter dem stärksten Volumenknoten geschlossen haben
      return last_bar.close < last_bar.poc_price;
    }

    return false;
  }
};

class DeltaAbsorptionTrigger : public ITrigger {
private:
  double min_delta_threshold_;

public:
  DeltaAbsorptionTrigger(double min_delta) : min_delta_threshold_(min_delta) {}

  eSignal EvaluateCandle(const MarketContext &context) override {
    if (context.history.empty()) return eSignal::NONE;
    const Bar &bar = context.history.back();

    // 1. Delta der gesamten Kerze berechnen
    double total_delta = 0.0;
    for (int i = 0; i < MAX_GRID_LEVELS; i++) {
      if (bar.vap_grid[i].price > 0.0) {
        // Delta = Aggressive Käufer (Ask) - Aggressive Verkäufer (Bid)
        total_delta += (bar.vap_grid[i].ask_volume - bar.vap_grid[i].bid_volume);
      }
    }

    // 2. Absorption prüfen
    // BUY-Signal: Extremes negatives Delta (Verkaufsdruck), aber Kerze schließt grün (Käufer halten dagegen)
    if (total_delta <= -min_delta_threshold_ && bar.close > bar.open) {
      return eSignal::BUY;
    }
    
    // SELL-Signal: Extremes positives Delta (Kaufdruck), aber Kerze schließt rot
    if (total_delta >= min_delta_threshold_ && bar.close < bar.open) {
      return eSignal::SELL;
    }

    return eSignal::NONE;
  }
};

class OR_Trigger : public ITrigger {
private:
  std::vector<ITrigger*> triggers_;

public:
  void AddTrigger(ITrigger* trigger) {
    triggers_.push_back(trigger);
  }

  eSignal EvaluateCandle(const MarketContext &context) override {
    for (auto* t : triggers_) {
      eSignal signal = t->EvaluateCandle(context);
      // Der erste Trigger, der ein Signal (BUY/SELL) findet, gewinnt!
      if (signal != eSignal::NONE) {
        return signal; 
      }
    }
    return eSignal::NONE;
  }

  eSignal EvaluateTick(const MarketContext &context) override {
    for (auto* t : triggers_) {
      eSignal signal = t->EvaluateTick(context);
      if (signal != eSignal::NONE) return signal;
    }
    return eSignal::NONE;
  }
};

class AND_Trigger : public ITrigger {
private:
  std::vector<ITrigger*> triggers_;

public:
  void AddTrigger(ITrigger* trigger) {
    triggers_.push_back(trigger);
  }

  eSignal EvaluateCandle(const MarketContext &context) override {
    if (triggers_.empty()) return eSignal::NONE;

    //first trigger 
    eSignal consensus = triggers_[0]->EvaluateCandle(context);
    if (consensus == eSignal::NONE) return eSignal::NONE;

    //check consensus
    for (size_t i = 1; i < triggers_.size(); ++i) {
      if (triggers_[i]->EvaluateCandle(context) != consensus) {
        return eSignal::NONE; // Uneinigkeit! Trade wird abgebrochen.
      }
    }
    
    return consensus; // Alle sind sich einig
  }
};
