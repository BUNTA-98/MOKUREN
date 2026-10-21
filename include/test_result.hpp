#pragma once

#include "engine_state.hpp"   // WFAOOSConfig + TradeRecord (via ibroker.hpp) + json

#include <map>
#include <string>
#include <vector>

// backtest run results
struct TestResult {
  std::map<std::string, double> parameters;
  nlohmann::json full_config; 
  double net_profit;
  double max_drawdown; 
  int trades;
  double winrate;
  std::vector<TradeRecord> trade_log;
  std::vector<WFAOOSConfig> wfa_configs;
  
  double tp_pct = 0.0;
  double be_pct = 0.0;
  double sl_pct = 0.0;

  double sharpe = 0.0;
  double sortino = 0.0;
  double mc_drawdown = 0.0;

  // true if the prop guard halted new entries at any point in this run (daily
  // or total drawdown limit). a permutation that would have ended a funded
  // account must be visible in the results table and the CSV instead of being
  // ranked next to compliant runs as if it were tradeable.
  bool prop_breached = false;
};

// adv metrics
struct AdvancedMetrics {
    double sharpe = 0.0;
    double sortino = 0.0;
    double mc_dd = 0.0;
};
