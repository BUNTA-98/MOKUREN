#pragma once
#include "aggregator.hpp"
#include <vector>

struct MarketContext {
  const std::vector<Bar> &history;
  const Bar &live_bar;
  const Bar &htf_bar;
  
  SessionMetrics session; 
  // borrowed from the aggregator; avoids copying the 200-byte snapshot
  // into every MarketContext (one per tick / candle evaluation).
  const L2Snapshot &latest_l2; 
  const L2RingBuffer &l2_history; // <--- NEU: constant reference to the ring buffer
};
