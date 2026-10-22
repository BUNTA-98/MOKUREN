#pragma once

#include <cstdint>
#include <mutex>

namespace mokuren {

// Decides when an unusable book has waited long enough to justify reconnecting
// the socket.
//
// Re-subscribing to a topic that is already subscribed is documented to replay
// a snapshot, but a duplicate subscription is not something a new connection
// can promise. A fresh socket always sends one, because that is the handshake
// the first snapshot already relies on. So the escalation path has to exist,
// and it has to be testable without a network -- a reconnect storm on a venue
// outage is its own failure.
//
// Thread-safe: the streamer's callback thread invalidates and clears it, its
// heartbeat thread consults it.
class ResyncWatchdog {
 public:
  // first attempt after ~3s (a snapshot lands in milliseconds on a healthy
  // connection), backing off to 60s.
  explicit ResyncWatchdog(int64_t initial_delay_ms = 3000,
                          int64_t max_delay_ms = 60000);

  // the book became untrustworthy at `now_ms` (gap detected, or a reset).
  void OnBookInvalidated(int64_t now_ms);

  // a snapshot landed: nothing to do until the book is invalidated again.
  void OnSnapshotApplied();

  bool waiting() const;

  // true once the current window has elapsed. does not consume the window: the
  // caller performs the reconnect and must then call OnReconnectAttempted, or
  // a failed attempt would be retried at the same cadence forever.
  bool ShouldReconnect(int64_t now_ms) const;

  void OnReconnectAttempted(int64_t now_ms);

  int64_t deadline_ms() const;

 private:
  int64_t initial_delay_ms_;
  int64_t max_delay_ms_;
  int64_t delay_ms_;
  int64_t deadline_ms_ = 0;
  bool waiting_ = false;

  mutable std::mutex mutex_;
};

}  // namespace mokuren
