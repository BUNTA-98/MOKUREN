#include "analyzer.hpp"
#include "trade_event.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>








PriceLevel* Analyzer::GetOrAddLevel(Bar& bar, double price){

  
  double normalized_price = std::floor(price / bar.tick_size) * bar.tick_size;
  
  //bei leerer bar
  if (bar.active_levels == 0){
    bar.base_price = normalized_price - (MAX_GRID_LEVELS / 2) * bar.tick_size;
    bar.lowest_price = normalized_price;
  } 
  
  //wenn bereits level besteht
  //calc index
  int index = std::round((normalized_price - bar.base_price) / bar.tick_size);
  
  //check bounds
  if (index >= 0 && index < MAX_GRID_LEVELS){

    //initialize price level
    if (bar.vap_grid[index].price == 0.0){
      
      bar.vap_grid[index].price = normalized_price;
      bar.vap_grid[index].bid_volume = 0.0;
      bar.vap_grid[index].ask_volume = 0.0;
      
      bar.active_levels++;

    }

    return &bar.vap_grid[index];
    
  }

  std::cout << "price level out of range";
  return nullptr;

}


void Analyzer::UpdateBarData(Bar& bar, const TradeEvent& trade){

  if (bar.open == 0.0){
  
    bar.open = trade.price;
    bar.close = trade.price;
    bar.high = trade.price;
    bar.low = trade.price;

  }
  else {
    
    bar.high  = std::max(bar.high, trade.price);
    bar.low   = std::min(bar.low, trade.price);
    bar.close = trade.price;
  
  }

  bar.total_volume += trade.quantity;

  PriceLevel* level_ptr = GetOrAddLevel(bar, trade.price);
  if (level_ptr){      //potential BUG

    if (trade.is_buyer_maker){
      level_ptr->bid_volume += trade.quantity;
      bar.cumulative_delta -= trade.quantity;
    }
    else {
      level_ptr->ask_volume += trade.quantity;
      bar.cumulative_delta += trade.quantity;
      
    }

  }
  else {
    
    std::cout << "ERROR: updateBarData() level_ptr.price !=0" << std::endl;
    return;
  }

}





eSignal Analyzer::AnalyzeCandle(Bar& bar, double ratio_threshold, double min_stacked_count){
  double max_vol = -1.0;    //frag nich
  double poc_price = 0.0;


  //calc POC
  for (int i = 0; i < MAX_GRID_LEVELS; i++){

    double total_vol = bar.vap_grid[i].bid_volume + bar.vap_grid[i].ask_volume;
    if (total_vol > max_vol && bar.vap_grid[i].price > 0.0){
      max_vol = total_vol;
      poc_price = bar.vap_grid[i].price;
    }
  }

  bar.poc_price = poc_price;



  //diagonal comp for imbalances (refactor so we dont have to iterate multiple times)
  int current_buy_stacked = 0;
  int max_buy_stacked = 0;

  int current_sell_stacked = 0;
  int max_sell_stacked = 0;


  for (int i = 1; i < MAX_GRID_LEVELS; i++){
    
    auto& upper = bar.vap_grid[i];
    auto& lower = bar.vap_grid[i - 1];

    //check buy imbalance (ask upper vs bid lower)
    if (lower.bid_volume > 0.0 && upper.ask_volume >= lower.bid_volume * ratio_threshold){
    
      current_buy_stacked++;
      max_buy_stacked = std::max(max_buy_stacked, current_buy_stacked);

    }  else {
      
      current_buy_stacked = 0;
    
    }

    //check sell imbalance (bid lower vs ask upper)
    if (upper.ask_volume > 0.0 && lower.bid_volume >= upper.ask_volume * ratio_threshold){
    
      current_sell_stacked++;
      max_sell_stacked = std::max(max_sell_stacked, current_sell_stacked);
    
    } else {

      current_sell_stacked = 0;
    }
  }

  //generate signal based on imbalance
  if (max_buy_stacked >= min_stacked_count && max_sell_stacked < min_stacked_count){
    
    return eSignal::BUY;
  
  } else if (max_sell_stacked >= min_stacked_count && max_buy_stacked < min_stacked_count
  ){
    
    return eSignal::SELL;
  
  }

  return eSignal::NONE;

}



eSignal Analyzer::ProcessTrade(Bar& bar, TradeEvent& trade, int64_t interval_ns ){

  //init empty bar timestamp
  if (bar.timestamp_start == 0){
    bar.timestamp_start = trade.timestamp;
  }

  //time rollover (next candle)
  if (trade.timestamp >= (bar.timestamp_start + interval_ns)){
  
    eSignal signal = AnalyzeCandle(bar, 3, 3);
    bar.ResetBar(trade.timestamp);
    UpdateBarData(bar, trade);

    return signal;

  }

  //if trade belongs to active candle, just update it
  UpdateBarData(bar, trade);

  return eSignal::NONE;

}



