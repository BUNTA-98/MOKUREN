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

// results of a backtest run
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

// rolling window definition for walk-forward analysis
struct WFAWindow {
  int64_t in_sample_start;
  int64_t in_sample_end;
  int64_t out_of_sample_start;
  int64_t out_of_sample_end;
};

// advanced risk and performance metrics
struct AdvancedMetrics {
    double sharpe = 0.0;
    double sortino = 0.0;
    double mc_dd = 0.0;
};

// calculates sharpe, sortino and monte-carlo drawdown
AdvancedMetrics CalculateAdvancedMetrics(const std::vector<TradeRecord>& trades);

// main backtest and replay engine. handles multithreading, data loading and execution.
class Mokuren {
private:
  // loads trades from binary cache or parses csv
  std::vector<TradeEvent> LoadAllTrades(const std::string &dataset_path);
  
  // loads level 2 orderbook snapshots from binary file
  std::vector<L2Snapshot> LoadAllL2Snapshots(const std::string &dataset_path);

  // verifies time sync between trades and l2 data
  bool ValidateDataAlignment(const std::vector<TradeEvent>& trades, const std::vector<L2Snapshot>& l2, std::function<void(const std::string&)> on_status);

  // extracts a specific time window from trade data
  std::vector<TradeEvent> SliceTrades(const std::vector<TradeEvent>& source_trades, int64_t start_time, int64_t end_time);

  // extracts a specific time window from l2 data
  std::vector<L2Snapshot> SliceL2(const std::vector<L2Snapshot>& source_l2, int64_t start_time, int64_t end_time);

  // generates rolling windows for wfa
  std::vector<WFAWindow> GenerateWFAWindows(int64_t first_ts, int64_t last_ts, int64_t in_sample_ms, int64_t out_of_sample_ms, int64_t step_ms);

  // configures trade management like breakeven and trailing stops
  void ApplyRMultiplesManagement(const nlohmann::json& json_config, double base_sl_pct, EngineInstance& eng);

public:
  Mokuren() = default;

  // executes a full grid search optimization over all parameter combinations
  void RunGridSearch(const std::string &ignored_legacy_path, const std::string &config_path,
                     std::function<void(const std::string&)> on_status,
                     std::function<void(int, int)> on_progress,
                     std::function<void(const TestResult&)> on_result);

  // executes a walk-forward analysis (rolling optimization)
  void RunWFA(const std::string &ignored_legacy_path, const std::string &config_path,
              std::function<void(const std::string&)> on_status,
              std::function<void(int, int)> on_progress,
              std::function<void(const TestResult&)> on_result);

  // replays a single config to generate ui display data
  ReplayResult ReplaySingleRun(const std::string &ignored_legacy_path, const nlohmann::json& winning_config);

  // replays the out-of-sample periods of a wfa run for ui display
  ReplayResult ReplayWFA(const std::string &ignored_legacy_path, const std::vector<WFAOOSConfig>& wfa_configs);
};