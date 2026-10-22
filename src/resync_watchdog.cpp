#include "resync_watchdog.hpp"

#include <algorithm>

namespace mokuren {

ResyncWatchdog::ResyncWatchdog(int64_t initial_delay_ms, int64_t max_delay_ms)
    : initial_delay_ms_(initial_delay_ms > 0 ? initial_delay_ms : 1),
      max_delay_ms_(std::max(initial_delay_ms_, max_delay_ms)),
      delay_ms_(initial_delay_ms > 0 ? initial_delay_ms : 1) {}

void ResyncWatchdog::OnBookInvalidated(int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  waiting_ = true;
  deadline_ms_ = now_ms + delay_ms_;
}

void ResyncWatchdog::OnSnapshotApplied() {
  std::lock_guard<std::mutex> lock(mutex_);
  waiting_ = false;
  deadline_ms_ = 0;
  // a healthy snapshot resets the ladder, so a later blip does not inherit a
  // long backoff it did not earn.
  delay_ms_ = initial_delay_ms_;
}

bool ResyncWatchdog::waiting() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return waiting_;
}

bool ResyncWatchdog::ShouldReconnect(int64_t now_ms) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return waiting_ && now_ms >= deadline_ms_;
}

void ResyncWatchdog::OnReconnectAttempted(int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!waiting_) return;
  delay_ms_ = std::min(max_delay_ms_, delay_ms_ * 2);
  deadline_ms_ = now_ms + delay_ms_;
}

int64_t ResyncWatchdog::deadline_ms() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return deadline_ms_;
}

}  // namespace mokuren
