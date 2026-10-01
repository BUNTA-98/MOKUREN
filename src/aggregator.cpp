#include "aggregator.hpp"
#include "trade_event.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

PriceLevel *Aggregator::GetOrAddLevel(double price) {
  double normalized_price = std::floor(price / tick_size) * tick_size;

  if (active_levels == 0) {
    base_price = normalized_price - (MAX_GRID_LEVELS / 2) * tick_size;
    lowest_price = normalized_price;
  }

  int index = std::round((normalized_price - base_price) / tick_size);

  if (index >= 0 && index < MAX_GRID_LEVELS) {
    if (live_workspace[index].price == 0.0) {
      live_workspace[index].price = normalized_price;
      live_workspace[index].bid_volume = 0.0;
      live_workspace[index].ask_volume = 0.0;
      active_levels++;
    }
    return &live_workspace[index];
  }

  return nullptr;
}

void Aggregator::UpdateBarData(Bar &bar, const TradeEvent &trade) {
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

void Aggregator::AnalyzeCandle(Bar &bar) {
  double max_vol = -1.0; 
  double poc_price = 0.0;

  for (int i = 0; i < MAX_GRID_LEVELS; i++) {
    double total_vol = live_workspace[i].bid_volume + live_workspace[i].ask_volume;
    if (total_vol > max_vol && live_workspace[i].price > 0.0) {
      max_vol = total_vol;
      poc_price = live_workspace[i].price;
    }
  }

  bar.poc_price = poc_price;
}

bool Aggregator::ProcessTrade(Bar &live_bar, const TradeEvent &trade) {
  // vwap reset & calculation with offset
  int64_t offset_ms = vwap_reset_hour_ * 3600000LL;
  int64_t session_day = (trade.timestamp - offset_ms) / 86400000LL;

  if (session_day > current_session_day_) {
    session_vol_ = 0.0;
    session_price_vol_ = 0.0;
    current_session_day_ = session_day;
  }

  session_vol_ += trade.quantity;
  session_price_vol_ += (trade.price * trade.quantity);
  
  current_metrics_.vwap = session_vol_ > 0.0 ? (session_price_vol_ / session_vol_) : trade.price;
  current_metrics_.total_session_volume = session_vol_;
  
  // save to bar
  live_bar.vwap = current_metrics_.vwap;

  // --- original close logic ---
  if (next_close_time == 0) {
    int64_t start_time = trade.timestamp - (trade.timestamp % interval_ms);
    next_close_time = start_time + interval_ms;
    live_bar.ResetBar(start_time);
    ResetWorkspace(); 
  }

  bool candle_closed = false;

  while (trade.timestamp >= next_close_time) {
    AnalyzeCandle(live_bar);
    PackFootprint(live_bar); 
    history.push_back(live_bar);
    candle_closed = true;

    int64_t new_start = next_close_time;
    next_close_time += interval_ms;
    
    live_bar.ResetBar(new_start);
    ResetWorkspace(); 
  }

  UpdateBarData(live_bar, trade);

  return candle_closed;
}

void Aggregator::FlushLastCandle(Bar &live_bar) {
  if (live_bar.total_volume > 0.0) {
    AnalyzeCandle(live_bar);
    PackFootprint(live_bar); 
    history.push_back(live_bar);
  }
}