#pragma once
#include "aggregator.hpp"
#include <vector>

struct MarketContext {
  const std::vector<Bar> &history;
  const Bar &live_bar;
  const Bar &htf_bar;
  
  SessionMetrics session; 
  L2Snapshot latest_l2; 
  const L2RingBuffer &l2_history; // <--- NEU: constant reference to the ring buffer
};