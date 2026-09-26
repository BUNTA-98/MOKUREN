#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// single result for the terminal leaderboard
struct UIResult {
  std::string params_str;
  double net_profit;
  double winrate;
  int trades;
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

  // thread-safe setter for status string
  void SetStatus(const std::string& status) {
    std::lock_guard<std::mutex> lock(ui_mutex);
    status_text = status;
  }

  // thread-safe getter for status string
  std::string GetStatus() {
    std::lock_guard<std::mutex> lock(ui_mutex);
    return status_text;
  }
};