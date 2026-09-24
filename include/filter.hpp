
#pragma once
#include "aggregator.hpp"
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

class MinVolumeFilter : public IFilter {
private:
  double min_volume_;

public:
  MinVolumeFilter(double min_volume) : min_volume_(min_volume) {}

  bool AllowTrade(const MarketContext &context,
                  TradeSignal intended_signal) override {
    if (context.history.empty())
      return false;
    // Blockiert den Trade, wenn die letzte Kerze extrem wenig Volumen hatte
    // (Choppiness)
    return context.history.back().total_volume >= min_volume_;
  }
};

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
    if (context.history.empty())
      return false;

    // Unix-Timestamp der Kerze in lokale Zeit umwandeln
    std::time_t time_val = context.history.back().timestamp_start / 1000;
    std::tm *time_info = std::localtime(&time_val);

    int current_mins_of_day = time_info->tm_hour * 60 + time_info->tm_min;
    int start_mins_of_day = start_hour_ * 60 + start_min_;
    int end_mins_of_day = end_hour_ * 60 + end_min_;

    // Prüfen, ob die aktuelle Zeit im Zeitfenster liegt
    return (current_mins_of_day >= start_mins_of_day &&
            current_mins_of_day <= end_mins_of_day);
  }
};

class POCTrendFilter : public IFilter {
public:
  bool AllowTrade(const MarketContext &context,
                  TradeSignal intended_signal) override {
    if (context.history.empty())
      return false;

    const Bar &last_bar = context.history.back();

    // Wenn kein POC berechnet wurde (sollte nicht passieren), erlaube den Trade
    if (last_bar.poc_price == 0.0)
      return true;

    if (intended_signal.direction == SignalDirection::BUY) {
      // Nur kaufen, wenn wir über dem stärksten Volumenknoten geschlossen haben
      return last_bar.close > last_bar.poc_price;
    } else if (intended_signal.direction == SignalDirection::SELL) {
      // Nur shorten, wenn wir unter dem stärksten Volumenknoten geschlossen
      // haben
      return last_bar.close < last_bar.poc_price;
    }

    return false;
  }
};
