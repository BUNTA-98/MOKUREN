#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <nlohmann/json.hpp>
#include "ibroker.hpp" 

// map out of sample wfa window configs
struct WFAOOSConfig {
  int64_t oos_start;
  int64_t oos_end;
  nlohmann::json config;
};

// core trade data passed to ui renderers
struct TradeInfo {
    bool is_long;
    double entry_price;
    double exit_price;
    double pnl;
    size_t candle_idx;
    size_t exit_candle_idx;
    std::string exit_reason;
    std::vector<StopEvent> stop_history; // <--- holds all trailing/be events
};

// single result for the terminal leaderboard
struct UIResult {
  std::string params_str;
  nlohmann::json full_config; 
  double net_profit;
  double winrate;
  int trades;
  double max_drawdown;
  double tp_pct = 0.0;
  double be_pct = 0.0;
  double sl_pct = 0.0;
  
  double sharpe = 0.0;
  double sortino = 0.0;
  double mc_drawdown = 0.0;
    
  std::vector<TradeRecord> trade_log; 
  std::vector<WFAOOSConfig> wfa_configs; 
};

// shared memory bridge for notcurses dashboard
struct EngineState {
  std::atomic<bool> is_running{false};
  std::atomic<bool> is_finished{false};
  
  std::atomic<int> current_permutation{0};
  std::atomic<int> total_permutations{0};
  
  std::mutex ui_mutex;
  std::string status_text = "IDLE";
  std::vector<UIResult> top_results;

  void SetStatus(const std::string& status) {
    std::lock_guard<std::mutex> lock(ui_mutex);
    status_text = status;
  }

  std::string GetStatus() {
    std::lock_guard<std::mutex> lock(ui_mutex);
    return status_text;
  }
};