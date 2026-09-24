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

  double tick_size = 0.5;
  double base_price = 0.0;
  double lowest_price = 0.0;
  int active_levels = 0;

  PriceLevel vap_grid[MAX_GRID_LEVELS];

  void ResetBar(int64_t new_timestamp) {
    open = 0.0;
    high = 0.0;
    low = 0.0;
    close = 0.0;
    total_volume = 0.0;
    cumulative_delta = 0.0;
    poc_price = 0.0;

    timestamp_start = new_timestamp;

    base_price = 0.0;
    lowest_price = 0.0;
    active_levels = 0;

    for (int i = 0; i < MAX_GRID_LEVELS; i++) {
      vap_grid[i].price = 0.0;
      vap_grid[i].bid_volume = 0.0;
      vap_grid[i].ask_volume = 0.0;
    }
  }
};

class Aggregator {
public:
  Aggregator(int64_t interval, double tick)
      : interval_ms(interval), tick_size(tick) {}

  PriceLevel *GetOrAddLevel(Bar &bar, double price);

  bool ProcessTrade(Bar &live_bar, const TradeEvent &trade);

  void UpdateBarData(Bar &bar, const TradeEvent &trade);

  void AnalyzeCandle(Bar &bar); // kann raus??

  const std::vector<Bar> &GetHistory() const { return history; }

  void FlushLastCandle(Bar &live_bar);

private:
  int64_t interval_ms;
  double tick_size;
  int64_t next_close_time;

  std::vector<Bar> history;
};
