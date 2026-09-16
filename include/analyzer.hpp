#pragma once
#include <array>
#include <cstdint>
#include "trade_event.hpp"

constexpr int MAX_GRID_LEVELS = 2000;

struct PriceLevel{
  
  double price;
  double bid_volume;
  double ask_volume;

};



enum class eSignal {
  NONE,
  BUY,
  SELL
};

struct Bar{
  

  double timestamp_start;
  double tick_size;
  double base_price;    //den hier noch zu poc machen irgendwann
  double open;
  double close;
  double high;
  double low;
  
  double total_volume;
  double cumulative_delta;
  
  double lowest_price;
  double poc_price;

  
  int active_levels;
  
  std::array<PriceLevel, MAX_GRID_LEVELS> vap_grid;
 

  void InitBar(double open_price, double _tick_size){
    open  = open_price;
    high  = open_price;
    low   = open_price;
    close = open_price;
    
    tick_size = _tick_size;
    base_price = open_price - (MAX_GRID_LEVELS/2) * tick_size;
    total_volume = 0;
    active_levels = 0;


  }


  void ResetBar(double new_timestamp){

    timestamp_start = new_timestamp;
    open = close = high = low = 0.0;
    total_volume = 0.0;
    cumulative_delta = 0.0;
    lowest_price = 0.0;
    poc_price = 0.0;
    active_levels = 0;
    vap_grid.fill({}); // Löscht alle Level im std::array
  }
};


class Analyzer{

public:

  Analyzer() = default;
  
  eSignal ProcessTrade(Bar& bar, TradeEvent& trade, int64_t interval_ns);
  
  PriceLevel* GetOrAddLevel(Bar& bar, double price);
  
  void UpdateBarData(Bar& bar, const TradeEvent& trade);

  eSignal AnalyzeCandle(Bar& bar, double ratio_threshold, double min_stacked_count);
  

  double tick_size;
  double imbalance_ratio;
  int min_stacked;
  Bar bar_1m;
  Bar bar_5m;


};
