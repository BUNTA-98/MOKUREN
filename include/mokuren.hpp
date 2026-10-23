#pragma once
// Thin facade over the grid-search and walk-forward engines. The heavy
// orchestration lives in grid_runner.* / wfa_runner.*; the legacy re-exports
// below are kept so call sites that leaned on mokuren.hpp for these headers
// keep compiling.
#include "config_parser.hpp"
#include "csv_parser.hpp"
#include "factory.hpp"
#include "wfa_window.hpp"
#include "market_context.hpp"
#include "ui_footprint.hpp"
#include "replay_manager.hpp"
#include "engine_state.hpp"
#include "test_result.hpp"
#include "grid_runner.hpp"
#include "wfa_runner.hpp"

#include <map>
#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

// main engine orchestrator. owns the public entry points and delegates to the
// grid / wfa runners; it holds no strategy logic of its own.
class Mokuren {
public:
  Mokuren() = default;

  // runs multi-core grid search.
  //
  // `dataset_path` is the dataset the selected profile points at (resolved by
  // the scanner). empty means "use the config's environment.dataset", which is
  // what callers without a profile in hand need. an explicit path wins: the
  // two resolutions used to disagree, and the scanner's choice was dropped, so
  // a picked .mkn was silently replaced by the directory "data".
  void RunGridSearch(const std::string &dataset_path, const std::string &config_path,
                     std::function<void(const std::string&)> on_status,
                     std::function<void(int, int)> on_progress,
                     std::function<void(const TestResult&)> on_result);

  // runs walk-forward analysis. same dataset_path rule as RunGridSearch.
  void RunWFA(const std::string &dataset_path, const std::string &config_path,
              std::function<void(const std::string&)> on_status,
              std::function<void(int, int)> on_progress,
              std::function<void(const TestResult&)> on_result);

  // replays config for ui. the trade and book channels are read from the same
  // `dataset_path`; empty falls back to environment.dataset. returns an empty
  // result (and logs why) when the recording is damaged, so a bad dataset
  // cannot unwind through the TUI.
  ReplayResult ReplaySingleRun(const std::string &dataset_path, const nlohmann::json& winning_config);

  // replays wfa for ui. same dataset_path rule as ReplaySingleRun.
  ReplayResult ReplayWFA(const std::string &dataset_path, const std::vector<WFAOOSConfig>& wfa_configs);

private:
  mokuren::GridRunner grid_runner_;
  mokuren::WfaRunner wfa_runner_;
};
