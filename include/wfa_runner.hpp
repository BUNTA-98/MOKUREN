#pragma once

#include "replay_manager.hpp"
#include "test_result.hpp"
#include "wfa_window.hpp"

#include <functional>
#include <string>
#include <vector>

namespace mokuren {

// Owns the walk-forward sweep and its per-window replay. Two distinct entry
// points because they are invoked from different UI paths (scan vs replay).
class WfaRunner {
public:
  using StatusCallback   = std::function<void(const std::string&)>;
  using ProgressCallback = std::function<void(int, int)>;
  using ResultCallback   = std::function<void(const TestResult&)>;

  void RunWFA(const std::string& config_path,
              StatusCallback on_status,
              ProgressCallback on_progress,
              ResultCallback on_result);

  ReplayResult ReplayWFA(const std::vector<WFAOOSConfig>& wfa_configs);
};

} // namespace mokuren
