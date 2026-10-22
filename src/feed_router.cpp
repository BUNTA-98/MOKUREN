#include "feed_router.hpp"

#include "logger.hpp"

#include <chrono>
#include <utility>

namespace mokuren::integrity {
namespace {

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

FeedRouter::FeedRouter(std::string venue, VenueProfile profile,
                       ProbeCriteria criteria, int64_t start_ms)
    : gate_(std::move(venue), std::move(profile), criteria, start_ms) {
  // the running path enforces the same depth requirement the probe did. a
  // venue that passed already proved it publishes this depth, so a shallower
  // frame later is a venue change and the validator marks it fatal -- the
  // engine stops rather than trading against a book whose level 2 is a zeroed
  // slot. Disabled() passes min_l2_depth 0, leaving the ungated path unchanged.
  run_limits_.min_depth = criteria.min_l2_depth;
}

void FeedRouter::SetLiveSink(LiveSink sink) { live_ = std::move(sink); }

void FeedRouter::Attach(IMarketStreamer& streamer) {
  streamer.SetTradeCallback([this](const TradeEvent& t) { OnTrade(t); });
  streamer.SetL2Callback([this](const L2Snapshot& s) { OnL2(s); });
  streamer.SetFinishedCallback([this]() { OnFinished(); });
  streamer.SetErrorCallback(
      [this](const std::string& message) { OnError(message); });
}

bool FeedRouter::BeginRunning(int64_t now_ms) {
  if (running_.load(std::memory_order_acquire)) return true;
  if (gate_.Poll(now_ms) != ProbeState::Ready) return false;
  if (!live_.on_trade || !live_.on_l2) return false;

  // emitted while running_ is still false: a streamer frame that lands between
  // these two lines is dropped by the gate, so a real snapshot can never
  // overtake the sentinel and be wiped by it.
  live_.on_l2(MakeBookGap(now_ms));
  running_.store(true, std::memory_order_release);

  LOG_INFO("FeedRouter: startup checks passed ({} ms); engine armed",
           gate_.Report().elapsed_ms);
  return true;
}

void FeedRouter::OnTrade(const TradeEvent& t) {
  const int64_t now = NowMillis();

  if (running_.load(std::memory_order_acquire)) {
    const Verdict verdict = ValidateTrade(t, run_trade_ts_);
    if (verdict.fatal()) {
      Escalate("trades", verdict.code);
      return;
    }
    if (!verdict.ok()) {
      dropped_running_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (live_.on_trade) live_.on_trade(t);
    return;
  }

  gate_.ObserveTrade(t, now);
}

void FeedRouter::OnL2(const L2Snapshot& s) {
  const int64_t now = NowMillis();

  if (IsBookGap(s)) {
    // emitted on reconnect, unexpected close and transport error. during the
    // probe it is the reconnect counter; while running it is the engine's cue
    // to stop trusting the book until a real snapshot arrives.
    if (running_.load(std::memory_order_acquire)) {
      if (live_.on_l2) live_.on_l2(s);
    } else {
      gate_.ObserveBookGap(now);
    }
    return;
  }

  if (running_.load(std::memory_order_acquire)) {
    const Verdict verdict = ValidateL2(s, run_limits_, run_l2_ts_);
    if (verdict.fatal()) {
      Escalate("l2", verdict.code);
      return;
    }
    if (!verdict.ok()) {
      // a stale or crossed snapshot would regress the local book; the engine
      // is better off keeping the last good one.
      dropped_running_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (live_.on_l2) live_.on_l2(s);
    return;
  }

  gate_.ObserveL2(s, now);
}

void FeedRouter::OnFinished() {
  if (running_.load(std::memory_order_acquire)) {
    if (live_.on_finished) live_.on_finished();
    return;
  }
  gate_.ObserveStreamEnded(NowMillis());
}

void FeedRouter::OnError(const std::string& message) {
  if (running_.load(std::memory_order_acquire)) {
    if (live_.on_error) live_.on_error(message);
    return;
  }
  gate_.ObserveTransportError(message, NowMillis());
}

void FeedRouter::Escalate(const char* channel, Code code) {
  if (escalated_.exchange(true, std::memory_order_acq_rel)) return;
  const std::string message =
      std::string("data integrity: ") + channel + ": " + CodeName(code);
  LOG_ERROR("FeedRouter: {}", message);
  if (live_.on_error) live_.on_error(message);
}

}  // namespace mokuren::integrity
