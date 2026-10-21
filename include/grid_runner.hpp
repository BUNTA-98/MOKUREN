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

  void Run(const std::string& config_path,
           StatusCallback on_status,
           ProgressCallback on_progress,
           ResultCallback on_result);
};

} // namespace mokuren
