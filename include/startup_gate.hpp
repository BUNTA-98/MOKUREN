#pragma once

#include "probe_report.hpp"
#include "venue_profile.hpp"

#include <cstdint>
#include <mutex>
#include <string>

namespace mokuren::integrity {

struct ProbeCriteria {
  // sustained arrival rate per minute, NOT an absolute count. a fixed count is
  // either too strict at 03:00 or too lax at the open, and the same venue
  // differs by ~10x across instruments: one overnight run produced 259 trades
  // in 90 s -- a healthy feed -- and failed a 300-trade minimum. 0 disables
  // the rate check for that channel.
  //
  // a positive rate always requires at least one frame, so "rate enabled" can
  // never be satisfied by a channel that stayed silent.
  uint64_t min_trades_per_min = 60;
  uint64_t min_l2_frames_per_min = 120;

  // levels per side each accepted book frame must carry. must match the
  // deepest "levels" param among the configured triggers (StackedImbalance
  // defaults to 2), because a shallower frame makes those triggers index
  // zeroed slots. 0 disables the check.
  uint64_t min_l2_depth = 2;

  // the window the rate is measured over. a burst that produces a minute's
  // worth of frames in three seconds is not a steady feed, so the rate is
  // only judged once this much wall time has passed. must be <= timeout_ms.
  int64_t min_duration_ms = 5000;
  int64_t timeout_ms = 30000;
  uint32_t max_reconnects = 0;

  // seconds, not minutes: long enough for a rate to mean something, short
  // enough to iterate on.
  static ProbeCriteria Development() { return {}; }

  // arm on the first frame: no soak, no minimums, no timeout. this is what
  // LiveOrchestrator::Start() uses, so the plain entry point keeps the pre-gate
  // behaviour while still running every frame through the validator once
  // armed. an explicit Start(criteria) is what turns the checks on.
  static ProbeCriteria Disabled() {
    ProbeCriteria c;
    c.min_trades_per_min = 0;
    c.min_l2_frames_per_min = 0;
    c.min_l2_depth = 0;
    c.min_duration_ms = 0;
    c.timeout_ms = 0;
    c.max_reconnects = 0;
    return c;
  }

  // the production default: a one-minute soak with a single reconnect budget.
  // 60 trades/min is ~1/s, which BTCUSDT perp clears even overnight (the run
  // that failed the old 300-count bar was doing ~172/min).
  static ProbeCriteria Production() {
    ProbeCriteria c;
    c.min_trades_per_min = 60;
    c.min_l2_frames_per_min = 300;
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
