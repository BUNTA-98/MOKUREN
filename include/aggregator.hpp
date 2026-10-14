#pragma once
#include "trade_event.hpp"
#include <vector>

// base signal types for order engine
enum class SignalDirection { NONE, BUY, SELL };

// container for generated trade signal
struct TradeSignal {
  SignalDirection direction = SignalDirection::NONE;
  double entry_price = 0.0;
  double stop_loss = 0.0;
  double take_profit = 0.0;
  double volume = 0.0;
};

// static grid limit (footprint block)
constexpr int MAX_GRID_LEVELS = 5000;
// ring buffer size for l2 snapshots
constexpr size_t L2_RING_BUFFER_SIZE = 1024;

// o(1) memory for orderbook history
struct L2RingBuffer {
  L2Snapshot buffer[L2_RING_BUFFER_SIZE];
  size_t head = 0;
  size_t count = 0;
  
  // push new snapshot, override oldest if full
  void push(const L2Snapshot& l2) {
    buffer[head] = l2;
    head = (head + 1) % L2_RING_BUFFER_SIZE;
    if (count < L2_RING_BUFFER_SIZE) count++;
  }
  
  // get snapshot by age (0 = newest, 1 = previous, etc.)
  const L2Snapshot* get_history(size_t age_index) const {
    if (age_index >= count) return nullptr;
    size_t idx = (head + L2_RING_BUFFER_SIZE - 1 - age_index) % L2_RING_BUFFER_SIZE;
    return &buffer[idx];
  }
};

// single price level
struct PriceLevel {
  double price = 0.0;
  double bid_volume = 0.0;
  double ask_volume = 0.0;
};

// aggregated candle
struct Bar {
  double open = 0.0;
  double high = 0.0;
  double low = 0.0;
  double close = 0.0;

  double total_volume = 0.0;
  double cumulative_delta = 0.0;
  double poc_price = 0.0;
  double vwap = 0.0; 

  int64_t timestamp_start = 0;
  double tick_size = 0.0;

  std::vector<PriceLevel> footprint;
  L2Snapshot last_l2; 

  // reset for next period
  void ResetBar(int64_t new_timestamp) {
    open = 0.0;
    high = 0.0;
    low = 0.0;
    close = 0.0;
    total_volume = 0.0;
    cumulative_delta = 0.0;
    poc_price = 0.0;
    vwap = 0.0;
    timestamp_start = new_timestamp;
    footprint.clear();
  }
};

// session metrics
struct SessionMetrics {
  double vwap = 0.0;
  double total_session_volume = 0.0;
};

class Aggregator {
public:
  Aggregator(int64_t interval, double tick, int vwap_reset_hour = 0)
      : interval_ms(interval), tick_size(tick), vwap_reset_hour_(vwap_reset_hour) {
    ResetWorkspace();
  }

  PriceLevel *GetOrAddLevel(double price);
  bool ProcessTrade(Bar &live_bar, const TradeEvent &trade);
  void ProcessL2(const L2Snapshot &l2); 
  void UpdateBarData(Bar &bar, const TradeEvent &trade);
  void AnalyzeCandle(Bar &bar); 
  
  const std::vector<Bar> &GetHistory() const { return history; }
  SessionMetrics GetSessionMetrics() const { return current_metrics_; }
  // borrowed from the ring buffer (no second 200-byte copy kept around)
  // and returned by reference so MarketContext binds without copying.
  const L2Snapshot &GetLatestL2() const {
    const L2Snapshot *s = l2_history_.get_history(0);
    return s ? *s : EmptyL2();
  }
  const L2RingBuffer &GetL2History() const { return l2_history_; }
  void FlushLastCandle(Bar &live_bar);

private:
  int64_t interval_ms;
  double tick_size;
  int vwap_reset_hour_ = 0; 
  int64_t next_close_time = 0;

  std::vector<Bar> history;

  // static footprint workspace. zero-init on construction; the per-candle
  // reset only clears the span we actually touched (see TrackTouched).
  PriceLevel live_workspace[MAX_GRID_LEVELS]{};
  double base_price = 0.0;
  double lowest_price = 0.0;
  int active_levels = 0;

  // narrowest [min,max] index range touched during the current candle.
  // analyze / pack / reset walk only this range instead of the full
  // MAX_GRID_LEVELS (5000 * 24 B ~= 117 KB) grid, which otherwise thrashes
  // l1/l2 on every candle boundary.
  int min_touched_idx_ = MAX_GRID_LEVELS;
  int max_touched_idx_ = -1;

  // vwap state
  int64_t current_session_day_ = -1; 
  double session_vol_ = 0.0;
  double session_price_vol_ = 0.0;
  SessionMetrics current_metrics_;

  // l2 state. the ring buffer is the single source of truth for the
  // latest snapshot; a separate latest_l2_ copy just doubled the
  // per-snapshot copy cost.
  L2RingBuffer l2_history_{}; // <--- NEU: Der Puffer im Arbeitsspeicher

  // valid-but-empty snapshot returned before any l2 data has arrived.
  static const L2Snapshot &EmptyL2() {
    static const L2Snapshot empty{};
    return empty;
  }

  // reset grid. only the slots touched during the previous candle are
  // cleared; walking all MAX_GRID_LEVELS slots on every candle boundary
  // was pure cache thrash.
  void ResetWorkspace() {
    base_price = 0.0;
    lowest_price = 0.0;
    active_levels = 0;
    if (max_touched_idx_ >= min_touched_idx_) {
      for (int i = min_touched_idx_; i <= max_touched_idx_; i++) {
        live_workspace[i].price = 0.0;
        live_workspace[i].bid_volume = 0.0;
        live_workspace[i].ask_volume = 0.0;
      }
    }
    min_touched_idx_ = MAX_GRID_LEVELS;
    max_touched_idx_ = -1;
  }

  // widen the tracked touched-index range
  void TrackTouched(int idx) {
    if (idx < min_touched_idx_) min_touched_idx_ = idx;
    if (idx > max_touched_idx_) max_touched_idx_ = idx;
  }

  // pack footprint for history. only the touched range is scanned.
  void PackFootprint(Bar &bar) {
    bar.footprint.clear();
    bar.footprint.reserve(active_levels);
    for (int i = min_touched_idx_; i <= max_touched_idx_; i++) {
      if (live_workspace[i].bid_volume > 0 || live_workspace[i].ask_volume > 0) {
        bar.footprint.push_back(live_workspace[i]);
      }
    }
  }
};
