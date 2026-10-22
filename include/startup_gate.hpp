#pragma once

#include "probe_report.hpp"
#include "venue_profile.hpp"

#include <cstdint>
#include <mutex>
#include <string>

namespace mokuren::integrity {

struct ProbeCriteria {
  uint64_t min_trades = 100;
  uint64_t min_l2_frames = 20;

  // levels per side each accepted book frame must carry. must match the
  // deepest "levels" param among the configured triggers (StackedImbalance
  // defaults to 2), because a shallower frame makes those triggers index
  // zeroed slots. 0 disables the check.
  uint64_t min_l2_depth = 2;

  // the counts must be met *and* held for this long: 200 frames in 3 s is not
  // a soak. must be smaller than timeout_ms.
  int64_t min_duration_ms = 2000;
  int64_t timeout_ms = 20000;
  uint32_t max_reconnects = 0;

  // a couple of seconds: fast enough to iterate on, long enough to prove both
  // channels actually produce.
  static ProbeCriteria Development() { return {}; }

  // arm on the first frame: no soak, no minimums, no timeout. this is what
  // LiveOrchestrator::Start() uses, so the plain entry point keeps the pre-gate
  // behaviour while still running every frame through the validator once
  // armed. an explicit Start(criteria) is what turns the checks on.
  static ProbeCriteria Disabled() {
    ProbeCriteria c;
    c.min_trades = 0;
    c.min_l2_frames = 0;
    c.min_l2_depth = 0;
    c.min_duration_ms = 0;
    c.timeout_ms = 0;
    c.max_reconnects = 0;
    return c;
  }

  // the production default: a one-minute soak with a single reconnect budget.
  static ProbeCriteria Production() {
    ProbeCriteria c;
    c.min_trades = 300;
    c.min_l2_frames = 100;
    c.min_duration_ms = 60000;
    c.timeout_ms = 90000;
    c.max_reconnects = 1;
    return c;
  }
};

// Decides whether a venue feed is fit to run on. Fed by the FeedRouter, polled
// by the owner; it never touches a socket, a clock or a log file, so the whole
// policy is testable by pushing fixtures through it.
//
// Contract: Ready requires EVERY channel the profile marks required to meet
// its own minimum on clean frames. A partial pass -- trades fine, l2 not -- is
// a failure, and a channel that delivers nothing fails on the deadline rather
// than passing by silence.
class StartupGate {
 public:
  StartupGate(std::string venue, VenueProfile profile, ProbeCriteria criteria,
              int64_t start_ms);

  // called from the streamer thread. each returns the state afterwards.
  ProbeState ObserveTrade(const TradeEvent& t, int64_t now_ms);
  ProbeState ObserveL2(const L2Snapshot& s, int64_t now_ms);
  ProbeState ObserveBookGap(int64_t now_ms);
  ProbeState ObserveTransportError(const std::string& message, int64_t now_ms);
  ProbeState ObserveStreamEnded(int64_t now_ms);

  // called from the owner thread to enforce min_duration_ms / timeout_ms.
  ProbeState Poll(int64_t now_ms);

  ProbeState state() const;
  ProbeReport Report() const;

 private:
  ProbeState EvaluateLocked(int64_t now_ms);
  ProbeState FailLocked(std::string reason);
  std::string ShortfallLocked() const;

  mutable std::mutex mutex_;
  VenueProfile profile_;
  ProbeCriteria criteria_;
  int64_t start_ms_;
  ProbeState state_ = ProbeState::Probing;
  ProbeReport report_;

  int64_t last_trade_ts_ = 0;
  int64_t last_l2_ts_ = 0;

  // built once from criteria_.min_l2_depth: every frame the probe accepts must
  // have cleared the same depth requirement the strategy will depend on.
  BookLimits l2_limits_;
};

}  // namespace mokuren::integrity
