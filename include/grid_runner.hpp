#pragma once

#include "test_result.hpp"

#include <functional>
#include <string>

namespace mokuren {

// Owns the multi-core grid-search sweep. Pure orchestration: it composes
// SignalExecutor + EngineInstance + DataManager and reports back through the
// three callbacks the Mokuren facade exposes.
class GridRunner {
public:
  using StatusCallback   = std::function<void(const std::string&)>;
  using ProgressCallback = std::function<void(int, int)>;
  using ResultCallback   = std::function<void(const TestResult&)>;

  // `dataset_path` is the dataset the caller resolved (the scanner hands over
  // the path from the selected profile). Empty means "read the config", which
  // is what callers without a profile in hand need.
  //
  // The override exists because the two resolutions used to disagree: the
  // scanner sniffed filepath/data_path/dataset, then the grid re-read
  // environment.dataset -- so a .mkn the operator picked was silently replaced
  // by the directory "data", and the failure looked like "no trades found".
  void Run(const std::string& config_path,
           StatusCallback on_status,
           ProgressCallback on_progress,
           ResultCallback on_result,
           const std::string& dataset_path = "");
};

} // namespace mokuren
