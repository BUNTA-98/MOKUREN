#pragma once
#include "market_data/format.hpp"

#include <cstdint>

// Compile-time ceiling on per-side depth, shared with the on-disk format so
// the two cannot drift. A dataset may publish less; L1 is depth 1 through the
// same code path, there is no variant type. Venues deeper than this bound are
// truncated and recorded as source_depth vs stored_depth in the metadata.
constexpr int MAX_L2_DEPTH = static_cast<int>(mokuren::mdf::kMaxBookDepth);

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

// kompakter binär-dump aus dem python l2-converter (flat memory block).
//
// No #pragma pack here. Packing a struct of doubles to byte 1 while one of
// its members demands 16-byte alignment is contradictory: it strips the
// padding the arrays need, forces unaligned 16-byte loads and let the record
// size fall out at 200. The explicit layout is:
//   8 (timestamp) + 32 (legacy L1) + 8 pad + 20*16 (bids) + 20*16 (asks)
//   = 688 bytes, 16-byte aligned, so every L2Level access is aligned.
// The record size changed, so the on-disk cache is incompatible with
// L2_cache_v2.bin; the reader filename was bumped to v3 in the same change.
struct alignas(16) L2Snapshot {
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
static_assert(sizeof(L2Level) == 16, "L2Level must stay 16 bytes");
static_assert(alignof(L2Snapshot) == 16, "L2Snapshot must be 16-byte aligned");
static_assert(sizeof(L2Snapshot) == 688,
              "L2Snapshot layout changed -- bump the L2 cache filename again");
