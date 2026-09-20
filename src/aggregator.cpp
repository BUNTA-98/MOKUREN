#include "aggregator.hpp"
#include "trade_event.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <vector>








PriceLevel* Aggregator::GetOrAddLevel(Bar& bar, double price){

  
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
  std::cout << "\n[CRITICAL] Price level out of range!" 
              << "\n -> Aktueller Preis: " << price 
              << "\n -> Base Price: " << bar.base_price 
              << "\n -> Berechneter Index: " << index 
              << "\n -> Tick Size: " << bar.tick_size << std::endl;
  return nullptr;
  return nullptr;

}


void Aggregator::UpdateBarData(Bar& bar, const TradeEvent& trade){

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





void Aggregator::AnalyzeCandle(Bar& bar){
  
  // std::cout << "[DEBUG] Kerze fertig! Aktive Level: " << bar.active_levels 
  //           << " | Total Vol: " << bar.total_volume << std::endl;


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

}



bool Aggregator::ProcessTrade(Bar& live_bar, const TradeEvent& trade, std::vector<Bar>& history) {
    // 1. Setup beim allerersten Tick des gesamten Backtests
    if (next_close_time == 0) {
        int64_t start_time = trade.timestamp - (trade.timestamp % interval_ms);
        next_close_time = start_time + interval_ms;
        live_bar.ResetBar(start_time);
        live_bar.tick_size = tick_size;
    }

    bool candle_closed = false;

    // 2. Zeit-Check: Liegt der Tick außerhalb unserer aktuellen Kerze?
    while (trade.timestamp >= next_close_time) {
        AnalyzeCandle(live_bar);
        
        // Aggregator archiviert die Kerze selbst!
        history.push_back(live_bar); 
        candle_closed = true;

        // Timer hochzählen und leere neue Kerze starten
        int64_t new_start = next_close_time;
        next_close_time += interval_ms;
        live_bar.ResetBar(new_start);
        live_bar.tick_size = tick_size;
    }

    // 3. Den aktuellen Tick in die korrekte (laufende oder frisch erstellte) Kerze buchen
    UpdateBarData(live_bar, trade);
    
    return candle_closed; 
}


/* bool Aggregator::ProcessTrade(Bar& bar, TradeEvent& trade, int64_t interval_ns) {
    if (bar.timestamp_start == 0) {
        // Exakt auf den Intervall-Anfang runden (z.B. glatte Minute)
        bar.timestamp_start = (trade.timestamp / interval_ns) * interval_ns;
    }

    // Prüfen, ob der Trade in die nächste Kerze fällt
    if (trade.timestamp >= (bar.timestamp_start + interval_ns)) {
        AnalyzeCandle(bar); 
        
        std::cout << " | O: " << bar.open                   
                  << " | C: " << bar.close 
                  << " | Vol: " << bar.total_volume 
                  << " | POC: " << bar.poc_price << std::endl;
                  
        return true; // Kerze ist fertig!
    }

    // Gehört zur aktuellen Kerze -> Daten aktualisieren
    UpdateBarData(bar, trade);
    return false;
}
*/
