#include "analyzer.hpp"
#include <math>
#include <algorithm>



 PriceLevel& GetOrAddLevel(Bar& bar, double price){

  
  double normalized_price = std::floor(price / tick_size) * tick_size;
  
  
  if (bar.active_levels == 0){
  
    bar.min_price = normalized_price;
    bar.vap_grid[0].price = normalized_price;
    bar.vap_grid[0].bid_volume = 0.0;
    bar.vap_grid[0].bid_volume = 0.0;
    bar.active_levels = 1;

    return bar.vap_grid[0]; 

  } 
  
  //calc index
  int index = std::round((normalized_price - bar.min_price) / tick_size);
  
  //check bounds
  if (index >= 0 && index < bar.max_level_count){

    //initialize price level
    if (bar.vap_grid[index] == 0.0){
      
      bar.vap_grid[index].price = normalized_price;
      bar.vap_grid[index].bid_volume = 0.0;
      bar.vap_grid[index].ask_volume = 0.0;
      bar.active_levels_count = std::max(bar.active_levels_count, index + 1);

      return bar.vap_grid[index];

    }

    std::cout << "price level out of range!" << std::endl; 
    return NULL;
  }

}


void updateBarData(Bar& bar, const TradeEvent& trade){


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

  bar.total_volume = trade.quantity;

  level_ptr = GetOrAddLevel(bar, trade);
  if (level_ptr =/ NULL){

    if (trade.is_buyer_maker){
      level_ptr.bid_volume += trade.quantity;
      bar.cumulative_delta -= trade.quantity;
    }
    else {
      level_ptr.ask_volume += trade.quantity;
      bar.cumulative_delta += trade.quantity;
      
    }

  }
  else {
    
    std::cout << "ERROR: updateBarData() level_ptr = NULL" << std::endl;
  
  }

}


