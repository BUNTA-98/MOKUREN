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

private:
  mokuren::GridRunner grid_runner_;
  mokuren::WfaRunner wfa_runner_;
};
