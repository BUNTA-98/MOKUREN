#pragma once
#include "trade_event.hpp"
#include <vector>

enum class SignalDirection { NONE, BUY, SELL };

struct TradeSignal {
  SignalDirection direction = SignalDirection::NONE;
  double entry_price = 0.0;
  double stop_loss = 0.0;
  double take_profit = 0.0;
  double volume = 0.0;
};

constexpr int MAX_GRID_LEVELS = 5000;

struct PriceLevel {
  double price = 0.0;
  double bid_volume = 0.0;
  double ask_volume = 0.0;
};

struct Bar {
  double open = 0.0;
  double high = 0.0;
  double low = 0.0;
  double close = 0.0;

  double total_volume = 0.0;
  double cumulative_delta = 0.0;
  double poc_price = 0.0;

  int64_t timestamp_start = 0;
  double tick_size = 0.0;

  // NEU: Der Vektor für den komprimierten Footprint
  std::vector<PriceLevel> footprint;

  void ResetBar(int64_t new_timestamp) {
    open = 0.0;
    high = 0.0;
    low = 0.0;
    close = 0.0;
    total_volume = 0.0;
    cumulative_delta = 0.0;
    poc_price = 0.0;
    timestamp_start = new_timestamp;
    footprint.clear();
  }
};

class Aggregator {
public:
  Aggregator(int64_t interval, double tick)
      : interval_ms(interval), tick_size(tick) {
    ResetWorkspace();
  }

  // Neue Signatur ohne "Bar &bar"
  PriceLevel *GetOrAddLevel(double price);

  bool ProcessTrade(Bar &live_bar, const TradeEvent &trade);

  void UpdateBarData(Bar &bar, const TradeEvent &trade);

  void AnalyzeCandle(Bar &bar); 

  const std::vector<Bar> &GetHistory() const { return history; }

  void FlushLastCandle(Bar &live_bar);

private:
  int64_t interval_ms;
  double tick_size;
  int64_t next_close_time = 0;

  std::vector<Bar> history;

  PriceLevel live_workspace[MAX_GRID_LEVELS];
  double base_price = 0.0;
  double lowest_price = 0.0;
  int active_levels = 0;

  void ResetWorkspace() {
    base_price = 0.0;
    lowest_price = 0.0;
    active_levels = 0;
    for (int i = 0; i < MAX_GRID_LEVELS; i++) {
      live_workspace[i].price = 0.0;
      live_workspace[i].bid_volume = 0.0;
      live_workspace[i].ask_volume = 0.0;
    }
  }

  void PackFootprint(Bar &bar) {
    bar.footprint.clear();
    bar.footprint.reserve(active_levels);
    for (int i = 0; i < MAX_GRID_LEVELS; i++) {
      if (live_workspace[i].bid_volume > 0 || live_workspace[i].ask_volume > 0) {
        bar.footprint.push_back(live_workspace[i]);
      }
    }
  }
};
