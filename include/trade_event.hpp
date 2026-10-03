#pragma once
#include <cstdint>

// roher trade tick (footprint / preis-aktion)
struct alignas(32) TradeEvent {
  int64_t timestamp;
  double price;
  double quantity;
  bool is_buyer_maker; // true = aggressiver verkauf (bid hit)
};

// kompakter binär-dump aus dem python l2-converter
#pragma pack(push, 1)
struct L2Snapshot {
    int64_t timestamp;      // 8 bytes (transaction_time)
    double best_bid_price;  // 8 bytes
    double best_bid_qty;    // 8 bytes
    double best_ask_price;  // 8 bytes
    double best_ask_qty;    // 8 bytes
}; 
#pragma pack(pop)