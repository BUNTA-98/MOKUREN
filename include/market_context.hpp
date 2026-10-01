#pragma once
#include "aggregator.hpp"
#include <vector>

struct MarketContext {
  const std::vector<Bar> &history;
  const Bar &live_bar;
  const Bar &htf_bar;
  
  // neu: zugriff auf vwap und value area
  SessionMetrics session; 
};