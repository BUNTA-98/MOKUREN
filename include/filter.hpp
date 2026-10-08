#pragma once
#include "aggregator.hpp"
#include "logger.hpp"
#include "market_context.hpp"
#include <algorithm>
#include <ctime>
#include <vector>

class IFilter {
public:
  virtual ~IFilter() = default;
  virtual bool AllowTrade(const MarketContext &context,
                          TradeSignal intended_signal) = 0;
};

// blocks trade if last candle volume is too low
class MinVolumeFilter : public IFilter {
private:
  double min_volume_;

public:
  MinVolumeFilter(double min_volume) : min_volume_(min_volume) {}

  bool AllowTrade(const MarketContext &context,
                  TradeSignal intended_signal) override {
    if (context.history.empty()) return false;
    return context.history.back().total_volume >= min_volume_;
  }
};

// blocks trade if relative volume (rvol) is below threshold
class RVOLFilter : public IFilter {
private:
  double rvol_threshold_;
  int period_;

public:
  RVOLFilter(double rvol_threshold = 1.5, int period = 20)
      : rvol_threshold_(rvol_threshold), period_(period) {}

  bool AllowTrade(const MarketContext &context, TradeSignal intended_signal) override {
    if (context.history.size() < period_ + 1) return false;

    double volume_sum = 0.0;
    size_t history_size = context.history.size();

    // calculate sma of previous candles
    for (size_t i = history_size - 1 - period_; i < history_size - 1; ++i) {
      volume_sum += context.history[i].total_volume;
    }

    double avg_volume = volume_sum / period_;
    if (avg_volume == 0.0) return false; 

    // check if current volume meets ratio
    double current_volume = context.history.back().total_volume;
    return (current_volume / avg_volume) >= rvol_threshold_;
  }
};

// blocks trade if average true range (atr) is below minimum volatility
class ATRFilter : public IFilter {
private:
  double min_atr_;
  int period_;

public:
  ATRFilter(double min_atr = 20.0, int period = 14)
      : min_atr_(min_atr), period_(period) {}

  bool AllowTrade(const MarketContext &context, TradeSignal intended_signal) override {
    if (context.history.size() < period_ + 1) return false;

    double atr_sum = 0.0;
    size_t size = context.history.size();

    // calc true range and sum it up
    for (size_t i = size - period_; i < size; ++i) {
      double high_low = context.history[i].high - context.history[i].low;
      double high_close = std::abs(context.history[i].high - context.history[i - 1].close);
      double low_close = std::abs(context.history[i].low - context.history[i - 1].close);
      
      atr_sum += std::max({high_low, high_close, low_close});
    }

    return (atr_sum / period_) >= min_atr_;
  }
};

// blocks trade if outside defined trading hours
class TimeOfDayFilter : public IFilter {
private:
  int start_hour_, start_min_;
  int end_hour_, end_min_;

public:
  TimeOfDayFilter(int start_h, int start_m, int end_h, int end_m)
      : start_hour_(start_h), start_min_(start_m), end_hour_(end_h),
        end_min_(end_m) {}

  bool AllowTrade(const MarketContext &context,
                  TradeSignal intended_signal) override {
    if (context.history.empty()) {
      LOG_TRACE("TimeOfDayFilter reject: empty history");
      return false;
    }

    // convert unix ms to local time
    std::time_t time_val = context.history.back().timestamp_start / 1000;
    struct tm time_info;
    if (localtime_r(&time_val, &time_info) == nullptr) {
      LOG_TRACE("TimeOfDayFilter reject: localtime_r failed for ts={}",
                context.history.back().timestamp_start);
      return false;
    }

    int current_mins = time_info.tm_hour * 60 + time_info.tm_min;
    int start_mins = start_hour_ * 60 + start_min_;
    int end_mins = end_hour_ * 60 + end_min_;

    bool allowed;
    if (start_mins <= end_mins) {
      // same-day session, e.g. 09:30 -> 16:00
      allowed = (current_mins >= start_mins && current_mins <= end_mins);
    } else {
      // overnight session crossing midnight, e.g. 22:00 -> 02:00
      allowed = (current_mins >= start_mins || current_mins <= end_mins);
    }

    if (!allowed) {
      LOG_TRACE("TimeOfDayFilter reject: now={:02d}:{:02d} window=[{:02d}:{:02d}-{:02d}:{:02d}] overnight={}",
                time_info.tm_hour, time_info.tm_min,
                start_hour_, start_min_, end_hour_, end_min_,
                start_mins > end_mins);
    }
    return allowed;
  }
};

// blocks trades against point of control (poc) trend
class POCTrendFilter : public IFilter {
public:
  bool AllowTrade(const MarketContext &context,
                  TradeSignal intended_signal) override {
    if (context.history.empty()) return false;

    const Bar &last_bar = context.history.back();
    if (last_bar.poc_price == 0.0) return true;

    // long above poc, short below poc
    if (intended_signal.direction == SignalDirection::BUY) {
      return last_bar.close > last_bar.poc_price;
    } else if (intended_signal.direction == SignalDirection::SELL) {
      return last_bar.close < last_bar.poc_price;
    }

    return false;
  }
};

// blocks trades against simple moving average trend
class MacroTrendFilter : public IFilter {
private:
  int period_;

public:
  MacroTrendFilter(int period = 200) : period_(period) {}

  bool AllowTrade(const MarketContext &context, TradeSignal signal) override {
    if (context.history.size() < period_) return false;

    // calc sma
    double sum = 0.0;
    for (size_t i = context.history.size() - period_; i < context.history.size(); ++i) {
      sum += context.history[i].close;
    }
    
    double sma = sum / period_;
    double current_price = context.history.back().close;

    // long above sma, short below sma
    if (signal.direction == SignalDirection::BUY && current_price < sma) return false;
    if (signal.direction == SignalDirection::SELL && current_price > sma) return false;

    return true;
  }
};

// blocks trade if average candle range is too tight
class VolatilityFilter : public IFilter {
private:
  double min_avg_range_;
  int period_;

public:
  VolatilityFilter(double min_range = 50.0, int period = 5)
      : min_avg_range_(min_range), period_(period) {}

  bool AllowTrade(const MarketContext &context,
                  TradeSignal intended_signal) override {
    if (context.history.size() < period_) return false;

    // calc average high-low range
    double total_range = 0.0;
    for (size_t i = context.history.size() - period_; i < context.history.size(); ++i) {
      total_range += (context.history[i].high - context.history[i].low);
    }

    return (total_range / period_) >= min_avg_range_;
  }
};

// blocks trades against daily session vwap
class VwapTrendFilter : public IFilter {
private:
  bool require_trend_alignment_;

public:
  VwapTrendFilter(bool require_trend_alignment = true) 
      : require_trend_alignment_(require_trend_alignment) {}

  bool AllowTrade(const MarketContext &context, TradeSignal intended_signal) override {
    if (!require_trend_alignment_) return true;
    if (intended_signal.direction == SignalDirection::NONE) return false;

    double vwap = context.session.vwap;
    if (vwap == 0.0) return true;

    double current_price = context.live_bar.close;

    // long above vwap, short below vwap
    if (intended_signal.direction == SignalDirection::BUY && current_price < vwap) return false;
    if (intended_signal.direction == SignalDirection::SELL && current_price > vwap) return false;

    return true;
  }
};

// blocks trades during price and cumulative delta divergence
class CVDDivergenceFilter : public IFilter {
private:
  int lookback_;

public:
  CVDDivergenceFilter(int lookback = 5) : lookback_(lookback) {}

  bool AllowTrade(const MarketContext &context, TradeSignal intended_signal) override {
    if (context.history.size() < lookback_ + 1) return false;

    size_t current_idx = context.history.size() - 1;
    size_t start_idx = current_idx - lookback_;

    double price_change = context.history[current_idx].close - context.history[start_idx].close;

    // sum up cvd
    double cumulative_delta = 0.0;
    for (size_t i = start_idx + 1; i <= current_idx; ++i) {
      cumulative_delta += context.history[i].cumulative_delta;
    }

    // block long if price goes up but delta goes down
    if (intended_signal.direction == SignalDirection::BUY && price_change > 0 && cumulative_delta < 0) return false; 
    
    // block short if price goes down but delta goes up
    if (intended_signal.direction == SignalDirection::SELL && price_change < 0 && cumulative_delta > 0) return false; 

    return true; 
  }
};

// blocks trade if l2 orderbook imbalance is too low
class OrderbookImbalanceFilter : public IFilter {
private:
    double required_ratio_; 

public:
    OrderbookImbalanceFilter(double ratio = 3.0) : required_ratio_(ratio) {}

    bool AllowTrade(const MarketContext& context, TradeSignal intended_signal) override {
        const auto& l2 = context.latest_l2;
        
        if (intended_signal.direction == SignalDirection::BUY) {
            // avoid division by zero
            if (l2.best_ask_qty <= 0.0) return true; 
            // check bid wall vs ask wall
            return (l2.best_bid_qty / l2.best_ask_qty) >= required_ratio_;
        } 
        else if (intended_signal.direction == SignalDirection::SELL) {
            if (l2.best_bid_qty <= 0.0) return true;
            // check ask wall vs bid wall
            return (l2.best_ask_qty / l2.best_bid_qty) >= required_ratio_;
        }
        
        return false;
    }
};
