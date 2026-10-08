#pragma once
#include "config_parser.hpp"
#include "csv_parser.hpp"
#include "factory.hpp"
#include "market_context.hpp"
#include "ui_footprint.hpp"
#include "replay_manager.hpp"
#include "engine_state.hpp"

#include <map>
#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

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
};

// wfa rolling window def
struct WFAWindow {
  int64_t in_sample_start;
  int64_t in_sample_end;
  int64_t out_of_sample_start;
  int64_t out_of_sample_end;
};

// adv metrics
struct AdvancedMetrics {
    double sharpe = 0.0;
    double sortino = 0.0;
    double mc_dd = 0.0;
};

// main engine orchestrator
class Mokuren {
private:
  // applies r-multiple risk management to paper trader
  void ApplyRMultiplesManagement(const nlohmann::json& json_config, double base_sl_pct, EngineInstance& eng);

public:
  Mokuren() = default;

  // runs multi-core grid search
  void RunGridSearch(const std::string &ignored_legacy_path, const std::string &config_path,
                     std::function<void(const std::string&)> on_status,
                     std::function<void(int, int)> on_progress,
                     std::function<void(const TestResult&)> on_result);

  // runs walk-forward analysis
  void RunWFA(const std::string &ignored_legacy_path, const std::string &config_path,
              std::function<void(const std::string&)> on_status,
              std::function<void(int, int)> on_progress,
              std::function<void(const TestResult&)> on_result);

  // replays config for ui
  ReplayResult ReplaySingleRun(const std::string &ignored_legacy_path, const nlohmann::json& winning_config);

  // replays wfa for ui
  ReplayResult ReplayWFA(const std::string &ignored_legacy_path, const std::vector<WFAOOSConfig>& wfa_configs);
};