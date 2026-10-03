#pragma once
#include "aggregator.hpp"
#include <vector>

struct MarketContext {
  const std::vector<Bar> &history;
  const Bar &live_bar;
  const Bar &htf_bar;
  
  SessionMetrics session; 
  L2Snapshot latest_l2; // <--- NEU: Die direkte Live-Leitung ins Orderbuch
};