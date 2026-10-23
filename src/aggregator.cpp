#include "aggregator.hpp"
#include "trade_event.hpp"
#include "live_debug.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// updates current l2 state and ring buffer memory
void Aggregator::ProcessL2(const L2Snapshot &l2) {
  LIVE_DBG("aggregator: ProcessL2 ts={} bid={} ask={}",
           l2.timestamp, l2.best_bid_price, l2.best_ask_price);
  // single copy: the ring buffer is now the only owner of the snapshot.
  l2_history_.push(l2); // <--- NEU: pushes snapshot into o(1) buffer
  ++l2_generation_;
}

// Copies the newest book into the bar, but only when it has changed since the
// previous copy.
//
// live_bar.last_l2 has to hold the newest snapshot at every evaluation, and
// ProcessTrade is where it is attached. The book moves far less often than the
// tape prints: even on a quiet session there are more trades than book frames
// (1264 vs 811 in recordings/btc_probe.mkn), and in a burst it is many prints
// per frame. Each of those was re-copying the identical 688-byte struct -- the
// largest per-print cost in the replay loop, larger than the ring push.
// Comparing a counter keeps the copy exact (the bytes would be identical) and
// removes all but the first.
//
// ResetBar() deliberately preserves last_l2, so the marker stays valid across a
// candle roll: the bar still holds the snapshot from bar_l2_generation_.
void Aggregator::AttachLatestL2(Bar &bar) {
  if (bar_l2_generation_ == l2_generation_) return;
  bar.last_l2 = GetLatestL2();
  bar_l2_generation_ = l2_generation_;
}

// gets price level from grid or creates new one.
// the grid is a fixed 5000-level window; when a print falls outside it the
// window slides instead of dropping the print (returning nullptr here silently
// lost volume and left the footprint empty).
PriceLevel *Aggregator::GetOrAddLevel(double price) {
  // snap to the nearest tick, not down to it. 0.1 is not exact in binary, so
  // floor(price / 0.1) maps two adjacent ticks into the same bucket about half
  // the time (85428.30 and 85428.20 both landed on 85428.2). for a stacked
  // imbalance that silently merges levels, which is exactly what the trigger
  // counts. llround is symmetric and keeps distinct ticks distinct.
  double normalized_price = std::llround(price / tick_size) * tick_size;

  // setup on first trade of candle. an empty touched range is exactly the
  // state after a reset, which is a more reliable signal than
  // active_levels == 0 (levels can slide out of the window mid-candle).
  if (max_touched_idx_ < min_touched_idx_) {
    base_price = normalized_price - (MAX_GRID_LEVELS / 2) * tick_size;
    lowest_price = normalized_price;
  }

  int index = static_cast<int>(std::llround((normalized_price - base_price) / tick_size));

  // slide the window if the print lands outside [0, MAX_GRID_LEVELS)
  if (index < 0 || index >= MAX_GRID_LEVELS) {
    // delta is added to every existing index; positive delta = move up
    const int delta = (index < 0) ? -index : -(index - (MAX_GRID_LEVELS - 1));
    const int shift = (delta < 0) ? -delta : delta;

    if (shift >= MAX_GRID_LEVELS) {
      // the jump exceeds the whole window: nothing survives, re-anchor.
      for (int i = min_touched_idx_; i <= max_touched_idx_; ++i) {
        if (live_workspace[i].price != 0.0) --active_levels;
        live_workspace[i].price = 0.0;
        live_workspace[i].bid_volume = 0.0;
        live_workspace[i].ask_volume = 0.0;
      }
      min_touched_idx_ = MAX_GRID_LEVELS;
      max_touched_idx_ = -1;
      base_price = normalized_price - (MAX_GRID_LEVELS / 2) * tick_size;
      lowest_price = normalized_price;
      index = MAX_GRID_LEVELS / 2;
    } else {
      if (delta > 0) {
        // drop levels leaving at the top, then move everything up
        for (int i = MAX_GRID_LEVELS - delta; i < MAX_GRID_LEVELS; ++i) {
          if (live_workspace[i].price != 0.0) --active_levels;
        }
        std::memmove(live_workspace + delta, live_workspace,
                     sizeof(PriceLevel) * (MAX_GRID_LEVELS - delta));
        for (int i = 0; i < delta; ++i) {
          live_workspace[i].price = 0.0;
          live_workspace[i].bid_volume = 0.0;
          live_workspace[i].ask_volume = 0.0;
        }
        base_price -= delta * tick_size;
      } else {
        // drop levels leaving at the bottom, then move everything down
        for (int i = 0; i < shift; ++i) {
          if (live_workspace[i].price != 0.0) --active_levels;
        }
        std::memmove(live_workspace, live_workspace + shift,
                     sizeof(PriceLevel) * (MAX_GRID_LEVELS - shift));
        for (int i = MAX_GRID_LEVELS - shift; i < MAX_GRID_LEVELS; ++i) {
          live_workspace[i].price = 0.0;
          live_workspace[i].bid_volume = 0.0;
          live_workspace[i].ask_volume = 0.0;
        }
        base_price += shift * tick_size;
      }

      // remap the touched span and clamp it to the surviving window.
      // dropped levels were already subtracted from active_levels above.
      if (max_touched_idx_ >= min_touched_idx_) {
        min_touched_idx_ = std::max(0, min_touched_idx_ + delta);
        max_touched_idx_ = std::min(MAX_GRID_LEVELS - 1, max_touched_idx_ + delta);
        if (max_touched_idx_ < min_touched_idx_) {
          min_touched_idx_ = MAX_GRID_LEVELS;
          max_touched_idx_ = -1;
        }
      }

      index += delta;
    }
  }

  // update level within static array
  if (index >= 0 && index < MAX_GRID_LEVELS) {
    if (live_workspace[index].price == 0.0) {
      live_workspace[index].price = normalized_price;
      live_workspace[index].bid_volume = 0.0;
      live_workspace[index].ask_volume = 0.0;
      active_levels++;
    }
    // remember the touched span so reset/analyze/pack stay in-range.
    TrackTouched(index);
    return &live_workspace[index];
  }

  return nullptr;
}

// writes raw trades into ohlc data and footprint grid
void Aggregator::UpdateBarData(Bar &bar, const TradeEvent &trade) {
  // the strategy layer reasons in ticks (stop placement, imbalance ladders).
  // carry the real tick size on the bar so triggers never re-infer it from
  // gapped footprint levels (that inference is always a multiple of the tick).
  bar.tick_size = tick_size;

  // track ohlc min/max
  if (bar.open == 0.0) {
    bar.open = trade.price;
    bar.close = trade.price;
    bar.high = trade.price;
    bar.low = trade.price;
  } else {
    bar.high = std::max(bar.high, trade.price);
    bar.low = std::min(bar.low, trade.price);
    bar.close = trade.price;
  }

  bar.total_volume += trade.quantity;

  // update footprint and delta
  PriceLevel *level_ptr = GetOrAddLevel(trade.price);
  if (level_ptr) { 
    if (trade.is_buyer_maker) {
      level_ptr->bid_volume += trade.quantity;
      bar.cumulative_delta -= trade.quantity;
    } else {
      level_ptr->ask_volume += trade.quantity;
      bar.cumulative_delta += trade.quantity;
    }
  }
}

// calculates poc (point of control) from aggregated footprint
void Aggregator::AnalyzeCandle(Bar &bar) {
  double max_vol = -1.0; 
  double poc_price = 0.0;

  for (int i = min_touched_idx_; i <= max_touched_idx_; i++) {
    double total_vol = live_workspace[i].bid_volume + live_workspace[i].ask_volume;
    if (total_vol > max_vol && live_workspace[i].price > 0.0) {
      max_vol = total_vol;
      poc_price = live_workspace[i].price;
    }
  }

  bar.poc_price = poc_price;
}

// main engine loop: builds history and controls candle slicing
bool Aggregator::ProcessTrade(Bar &live_bar, const TradeEvent &trade) {
  // calculate vwap offset and daily reset
  int64_t offset_ms = vwap_reset_hour_ * 3600000LL;
  int64_t session_day = (trade.timestamp - offset_ms) / 86400000LL;

  // reset session variables on new day
  if (session_day > current_session_day_) {
    session_vol_ = 0.0;
    session_price_vol_ = 0.0;
    current_session_day_ = session_day;
  }

  session_vol_ += trade.quantity;
  session_price_vol_ += (trade.price * trade.quantity);
  
  // calculate current vwap
  current_metrics_.vwap = session_vol_ > 0.0 ? (session_price_vol_ / session_vol_) : trade.price;
  current_metrics_.total_session_volume = session_vol_;
  
  live_bar.vwap = current_metrics_.vwap;
  
  // attach current l2 state to running candle (skipped when the book has not
  // moved since the previous attach -- see AttachLatestL2)
  AttachLatestL2(live_bar);

  // initialize timer
  if (next_close_time == 0) {
    int64_t start_time = trade.timestamp - (trade.timestamp % interval_ms);
    next_close_time = start_time + interval_ms;
    live_bar.ResetBar(start_time);
    ResetWorkspace(); 
    LIVE_DBG("aggregator: timer init trade_ts={} start_time={} next_close_time={} interval_ms={}",
             trade.timestamp, start_time, next_close_time, interval_ms);
  }

  bool candle_closed = false;

  // interval exceeded -> close candle
  while (trade.timestamp >= next_close_time) {
    // an interval without prints must still carry a valid price forward.
    // publishing open==0 poisons ATR/SMA/RVOL and every close-based trigger.
    if (live_bar.total_volume <= 0.0 && !history.empty()) {
      const double prev_close = history.back().close;
      live_bar.open = prev_close;
      live_bar.high = prev_close;
      live_bar.low = prev_close;
      live_bar.close = prev_close;
    }
    live_bar.tick_size = tick_size;

    AnalyzeCandle(live_bar);
    PackFootprint(live_bar); 
    history.push_back(live_bar);
    candle_closed = true;

    LIVE_DBG("aggregator: CANDLE CLOSED trade_ts={} closed_bar_ts={} o={} h={} l={} c={} vol={} history_size={}",
             trade.timestamp, live_bar.timestamp_start, live_bar.open, live_bar.high,
             live_bar.low, live_bar.close, live_bar.total_volume, history.size());

    int64_t new_start = next_close_time;
    next_close_time += interval_ms;
    
    live_bar.ResetBar(new_start);
    ResetWorkspace(); 
    
    // last_l2 is deliberately NOT re-attached here. ResetBar() preserves it
    // precisely so the first evaluation of a new candle still sees the book,
    // and no L2 frame can arrive inside ProcessTrade -- so GetLatestL2() would
    // return the snapshot already attached above, i.e. a redundant 688-byte
    // copy at every candle boundary.
  }

  // assign data to current timeframe
  UpdateBarData(live_bar, trade);

  return candle_closed;
}

// force flush at end of backtest
void Aggregator::FlushLastCandle(Bar &live_bar) {
  if (live_bar.total_volume > 0.0) {
    AnalyzeCandle(live_bar);
    PackFootprint(live_bar); 
    history.push_back(live_bar);
    LIVE_DBG("aggregator: FLUSH last candle ts={} vol={} history_size={}",
             live_bar.timestamp_start, live_bar.total_volume, history.size());
  }
}
