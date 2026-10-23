#include "startup_gate.hpp"

#include <cmath>
#include <sstream>
#include <utility>

namespace mokuren::integrity {
namespace {

void Record(ChannelReport& channel, const Verdict& verdict) {
  ++channel.seen;
  switch (verdict.severity) {
    case Severity::Ok:
      ++channel.delivered;
      break;
    case Severity::Recoverable:
      ++channel.recoverable;
      if (channel.first_recoverable == Code::Ok) {
        channel.first_recoverable = verdict.code;
      }
      break;
    case Severity::Fatal:
      ++channel.fatal;
      if (channel.first_fatal == Code::Ok) channel.first_fatal = verdict.code;
      break;
  }
}

}  // namespace

StartupGate::StartupGate(std::string venue, VenueProfile profile,
                         ProbeCriteria criteria, int64_t start_ms)
    : profile_(std::move(profile)),
      criteria_(criteria),
      start_ms_(start_ms) {
  report_.venue = std::move(venue);
  report_.trades.channel = "trades";
  report_.trades.required = profile_.trades_required;
  // min_required is not a fixed number any more: it is re-derived from the
  // rate at each evaluation, so a report row says how many frames the window
  // so far should have produced instead of a count tuned for one market hour.
  report_.trades.min_required = 0;
  report_.l2.channel = "l2";
  report_.l2.required = profile_.l2_required;
  report_.l2.min_required = 0;
  l2_limits_.min_depth = criteria_.min_l2_depth;
}

ProbeState StartupGate::ObserveTrade(const TradeEvent& t, int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != ProbeState::Probing) return state_;
  Record(report_.trades, ValidateTrade(t, last_trade_ts_));
  return EvaluateLocked(now_ms);
}

ProbeState StartupGate::ObserveL2(const L2Snapshot& s, int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != ProbeState::Probing) return state_;
  Record(report_.l2, ValidateL2(s, l2_limits_, last_l2_ts_));
  return EvaluateLocked(now_ms);
}

ProbeState StartupGate::ObserveBookGap(int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != ProbeState::Probing) return state_;
  // both streamers suppress the sentinel on the initial handshake, so this
  // only ever counts a genuine drop-and-recover.
  if (report_.trades.delivered + report_.l2.delivered > 0) {
    ++report_.reconnects;
  }
  return EvaluateLocked(now_ms);
}

ProbeState StartupGate::ObserveTransportError(const std::string& message,
                                              int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != ProbeState::Probing) return state_;
  report_.elapsed_ms = now_ms > start_ms_ ? now_ms - start_ms_ : 0;
  return FailLocked(std::string(CodeName(Code::TransportError)) + ": " +
                    message);
}

ProbeState StartupGate::ObserveStreamEnded(int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != ProbeState::Probing) return state_;
  // a clean end of input can still be a pass if it delivered enough first
  // (a short replay file, say); otherwise it is a failure, not a timeout.
  if (EvaluateLocked(now_ms) == ProbeState::Probing) {
    return FailLocked(CodeName(Code::StreamEndedEarly));
  }
  return state_;
}

ProbeState StartupGate::Poll(int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  return EvaluateLocked(now_ms);
}

ProbeState StartupGate::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

ProbeReport StartupGate::Report() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return report_;
}

ProbeState StartupGate::EvaluateLocked(int64_t now_ms) {
  if (state_ != ProbeState::Probing) return state_;

  const int64_t elapsed = now_ms > start_ms_ ? now_ms - start_ms_ : 0;
  report_.elapsed_ms = elapsed;

  // 1. a venue whose book can never satisfy the strategy is rejected before a
  //    single frame is examined. this is decidable from the profile alone, and
  //    it is the case that used to slip through: a depth-1 feed produces
  //    structurally valid snapshots, so every other rule passes while the
  //    depth triggers read zeroed level-2 slots.
  if (profile_.l2_required && profile_.l2_depth > 0 &&
      profile_.l2_depth < criteria_.min_l2_depth) {
    return FailLocked("l2: " + profile_.venue + " publishes " +
                      std::to_string(profile_.l2_depth) +
                      " book level(s) per side but the strategy needs " +
                      std::to_string(criteria_.min_l2_depth));
  }

  // 2. a fatal frame on a required channel ends the probe at once: the feed is
  //    not merely quiet, it is lying. waiting for the deadline would only add
  //    latency to the same verdict.
  if (profile_.trades_required && report_.trades.fatal > 0) {
    return FailLocked("trades: " +
                      std::string(CodeName(report_.trades.first_fatal)));
  }
  if (profile_.l2_required && report_.l2.fatal > 0) {
    return FailLocked("l2: " + std::string(CodeName(report_.l2.first_fatal)));
  }

  // 3. a soak that keeps dropping is not healthy even if it eventually
  //    delivers the counts.
  if (report_.reconnects > criteria_.max_reconnects) {
    return FailLocked("feed reconnected " + std::to_string(report_.reconnects) +
                      " time(s) during the checks (max " +
                      std::to_string(criteria_.max_reconnects) + ")");
  }

  // 4. sustained rate on every required channel, plus the minimum window.
  //    the rate is converted to a count for the window so far, which keeps
  //    ChannelReport::passed() meaningful and lets a row still say how many
  //    frames it needed.
  //
  //    a positive rate always demands at least one frame. without that floor
  //    a large rate multiplied by a small elapsed time rounds to zero, and a
  //    silent channel would "pass" -- the regression ChannelReport::passed()
  //    was written to prevent.
  const double minutes = static_cast<double>(elapsed) / 60000.0;
  auto required_count = [minutes](uint64_t per_min) -> uint64_t {
    if (per_min == 0) return 0;
    if (!(minutes > 0.0)) return 1;
    const auto scaled = static_cast<uint64_t>(
        std::ceil(static_cast<double>(per_min) * minutes));
    return scaled > 0 ? scaled : 1;
  };
  if (profile_.trades_required) {
    report_.trades.min_required =
        required_count(criteria_.min_trades_per_min);
  }
  if (profile_.l2_required) {
    report_.l2.min_required =
        required_count(criteria_.min_l2_frames_per_min);
  }

  const bool trades_ok =
      !profile_.trades_required ||
      report_.trades.delivered >= report_.trades.min_required;
  const bool l2_ok = !profile_.l2_required ||
                     report_.l2.delivered >= report_.l2.min_required;
  if (trades_ok && l2_ok && elapsed >= criteria_.min_duration_ms) {
    state_ = ProbeState::Ready;
    report_.state = ProbeState::Ready;
    report_.reason.clear();
    return state_;
  }

  // 5. deadline: partial success is failure.
  if (elapsed >= criteria_.timeout_ms) return FailLocked(ShortfallLocked());

  return ProbeState::Probing;
}

ProbeState StartupGate::FailLocked(std::string reason) {
  state_ = ProbeState::Failed;
  report_.state = ProbeState::Failed;
  report_.reason = std::move(reason);
  return state_;
}

std::string StartupGate::ShortfallLocked() const {
  std::ostringstream os;
  const double minutes = report_.elapsed_ms > 0
                             ? static_cast<double>(report_.elapsed_ms) / 60000.0
                             : 0.0;
  // the observed rate, not just the count. "259 frames" says nothing without
  // the window it arrived in, and the bare count is what made the old failure
  // read as a broken feed instead of a slow hour.
  auto observed_per_min = [minutes](uint64_t delivered) -> long long {
    if (!(minutes > 0.0)) return 0;
    return static_cast<long long>(static_cast<double>(delivered) / minutes);
  };

  os << "not enough sustained data after " << report_.elapsed_ms << " ms:";
  if (profile_.trades_required) {
    os << " trades " << report_.trades.delivered << " ("
       << observed_per_min(report_.trades.delivered) << "/min, need "
       << criteria_.min_trades_per_min << "/min)";
  }
  if (profile_.l2_required) {
    os << " l2 " << report_.l2.delivered << " ("
       << observed_per_min(report_.l2.delivered) << "/min, need "
       << criteria_.min_l2_frames_per_min << "/min)";
  }
  return os.str();
}

}  // namespace mokuren::integrity
