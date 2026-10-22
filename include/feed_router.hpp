#pragma once

#include "live_streamer.hpp"
#include "startup_gate.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace mokuren::integrity {

// The engine side of the router. Installed once the orchestrator exists and
// before BeginRunning(); nothing is forwarded while the gate is still probing,
// so installing it late cannot lose a frame the engine was entitled to.
struct LiveSink {
  std::function<void(const TradeEvent&)> on_trade;
  std::function<void(const L2Snapshot&)> on_l2;
  std::function<void()> on_finished;
  std::function<void(const std::string&)> on_error;
};

// Sits between one streamer and the engine, and is the only thing that decides
// whether a frame may reach the engine.
//
// The streamer is connected exactly once: Attach() replaces its callbacks
// before Start(), so the connection that passes the startup checks is the same
// connection that trades. There is no probe-then-reconnect gap, and a new
// venue needs no gate logic of its own -- writing a streamer is still all it
// takes, because the gate is wired from the outside.
class FeedRouter {
 public:
  FeedRouter(std::string venue, VenueProfile profile, ProbeCriteria criteria,
             int64_t start_ms);

  // Must be called on the owner thread before the streamer starts, so the
  // sink is published to the streamer thread by the thread that starts it.
  void SetLiveSink(LiveSink sink);

  // Installs this router as the streamer's callback target. Call once, before
  // Start().
  void Attach(IMarketStreamer& streamer);

  // Flip from probing to running. false if the gate is not Ready or no sink is
  // installed. Emits the book-gap sentinel to the engine while still probing,
  // so the engine waits for a fresh snapshot instead of trusting a book it
  // never saw built; a frame racing this call is dropped rather than arriving
  // before the sentinel.
  bool BeginRunning(int64_t now_ms);

  ProbeState Poll(int64_t now_ms) { return gate_.Poll(now_ms); }
  ProbeReport Report() const { return gate_.Report(); }

  bool running() const { return running_.load(std::memory_order_acquire); }

  // frames dropped after startup (recoverable-only), for monitoring.
  uint64_t DroppedWhileRunning() const {
    return dropped_running_.load(std::memory_order_relaxed);
  }

 private:
  void OnTrade(const TradeEvent& t);
  void OnL2(const L2Snapshot& s);
  void OnFinished();
  void OnError(const std::string& message);

  // report a fatal frame through the error channel the orchestrator already
  // listens on, instead of inventing a second failure path.
  void Escalate(const char* channel, Code code);

  StartupGate gate_;
  LiveSink live_;

  std::atomic<bool> running_{false};
  std::atomic<bool> escalated_{false};
  std::atomic<uint64_t> dropped_running_{0};

  // run-phase cursors. streamer thread only, so no lock: the gate owns its own
  // cursors and is finished with them once it reports Ready.
  int64_t run_trade_ts_ = 0;
  int64_t run_l2_ts_ = 0;
  BookLimits run_limits_{};
};

}  // namespace mokuren::integrity
