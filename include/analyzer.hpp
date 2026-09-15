#pragma once
#include <array>
#include <cstdint>
#include "trade_event.hpp"


struct PriceLevel{
  
  double price;
  double bid_volume;
  double ask_volume;

};



struct Bar{

  double timestamp_start;
  double open;
  double close;
  double high;
  double low;
  double total_volume;
  double cumulative_delta;
  double lowest_price;
  
  size_t active_levels;
  size_t max_level_count = 2000;
  
  std::array<PriceLevel, max_level_count> vap_grid;
 
  
  void ResetBar(double new_timestamp){

    timestamp_start = new_timestamp;
    open  = 0.0;
    close = 0.0;
    high  = 0.0;
    low   = 0.0;
    cumulative_delta    = 0;
    active_levels_count = 0;

  }
};


class Analyzer {

public:

  Analyzer(tick_size, imbalance_ratio, min_stacked);
  
  void processTrade(TradeEvent& event);


private:
  
  PriceLevel& GetOrAddLevel(Bar& bar, double price);
  
  void updateBarData(Bar& bar, const TradeEvent& trade)
  

  double tick_size;
  double imbalance_ratio;
  int min_stacked;
  Bar bar_1m;
  Bar bar_15m;


}
