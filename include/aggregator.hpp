#pragma once
#include "trade_event.hpp"
#include <cstddef>
#include <stdexcept>
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

  // reset for next period. tick_size and last_l2 deliberately survive: the
  // tick is constant for the run, and the first evaluation of the new candle
  // must still see the most recent book rather than a zeroed snapshot.
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

  // Insert-or-replace one level in the live footprint, keeping the vector
  // price-ordered. A tick-level trigger reads this while the candle is still
  // open, and the imbalance scan compares ADJACENT prices, so order is part of
  // the contract, not a nicety. Defined in aggregator.cpp.
  void UpsertFootprintLevel(const PriceLevel &level);
};

// session metrics
struct SessionMetrics {
  double vwap = 0.0;
  double total_session_volume = 0.0;
};

class Aggregator {
public:
  // interval and tick are the two divisors every downstream calculation
  // depends on. a zero here does not fail loudly: tick==0 turns every price
  // level into NaN and interval==0 spins the candle loop forever. reject them
  // at construction instead of discovering it as an empty footprint.
  Aggregator(int64_t interval, double tick, int vwap_reset_hour = 0)
      : interval_ms(interval), tick_size(tick), vwap_reset_hour_(vwap_reset_hour) {
    if (interval <= 0) {
      throw std::invalid_argument("Aggregator: interval must be > 0");
    }
    if (!(tick > 0.0)) {  // written this way so NaN is rejected too
      throw std::invalid_argument("Aggregator: tick_size must be > 0");
    }
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

  // immutable run constants. exposed so the startup capability gate and the
  // venue adapters can check a config without re-parsing the json.
  int64_t IntervalMs() const { return interval_ms; }
  double TickSize() const { return tick_size; }

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

  // set by GetOrAddLevel when it slides (or re-anchors) the grid: the bar's
  // live footprint then still mirrors prices the grid no longer holds, and
  // UpdateBarData prunes them before mirroring again.
  bool mirror_stale_ = false;

  // vwap state
  int64_t current_session_day_ = -1; 
  double session_vol_ = 0.0;
  double session_price_vol_ = 0.0;
  SessionMetrics current_metrics_;

  // l2 state. the ring buffer is the single source of truth for the
  // latest snapshot; a separate latest_l2_ copy just doubled the
  // per-snapshot copy cost.
  L2RingBuffer l2_history_{}; // <--- NEU: Der Puffer im Arbeitsspeicher

  // bumped by ProcessL2. AttachLatestL2 compares it against the value it last
  // copied into the bar and skips the copy when nothing has moved, because the
  // bytes it would write are identical.
  uint64_t l2_generation_ = 0;
  uint64_t bar_l2_generation_ = 0;

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

  // copy the newest book into the bar, but only when it changed since the last
  // copy. defined in aggregator.cpp: it needs the ring buffer and the
  // generation counter, and it is not worth inlining into the header.
  void AttachLatestL2(Bar &bar);

  // drop mirrored footprint levels the grid no longer holds after a window
  // slide or re-anchor. defined in aggregator.cpp.
  void PruneStaleFootprint(Bar &bar);

  // widen the tracked touched-index range
  void TrackTouched(int idx) {
    if (idx < min_touched_idx_) min_touched_idx_ = idx;
    if (idx > max_touched_idx_) max_touched_idx_ = idx;
  }

  // pack footprint for history. only the touched range is scanned. still the
  // authoritative pack for a closed bar: it rebuilds from the grid, so a live
  // mirror that drifted on a window slide cannot reach history.
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
