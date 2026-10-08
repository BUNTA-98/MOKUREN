#pragma once
#include <cstdint>

// maximale tiefe, die wir im ram vorhalten (cache line optimiert)
constexpr int MAX_L2_DEPTH = 5;

// roher trade tick (footprint / preis-aktion)
struct alignas(32) TradeEvent {
  int64_t timestamp;
  double price;
  double quantity;
  bool is_buyer_maker; // true = aggressiver verkauf (bid hit)
};

// einzelnes orderbuch-level (16 bytes)
struct alignas(16) L2Level {
  double price;
  double qty;
};

// kompakter binär-dump aus dem python l2-converter (flat memory block)
#pragma pack(push, 1)
struct L2Snapshot {
  int64_t timestamp;      
  
  // legacy level 1 (für schnelle O(1) abfragen der alten trigger)
  double best_bid_price;  
  double best_bid_qty;    
  double best_ask_price;  
  double best_ask_qty;    

  // zero-allocation arrays für die aggregierte tiefe
  L2Level bids[MAX_L2_DEPTH];
  L2Level asks[MAX_L2_DEPTH];
}; 
#pragma pack(pop)