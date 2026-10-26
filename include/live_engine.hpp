#pragma once
#include "aggregator.hpp"
#include "factory.hpp"
#include "feed_router.hpp"
#include "ibroker.hpp"
#include "live_streamer.hpp"
#include "logger.hpp"
#include "prop_guard.hpp"
#include "redis_publisher.hpp"
#include "signal_executor.hpp"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// orchestrates a live forward-test run:
//   - a streamer thread pushes trade/l2 events into a bounded queue
//   - a worker thread drains the queue and feeds the engine modules
//     in the same order as the historical backtest loop.
class LiveOrchestrator {
public:
  // hard cap on queued events. protects against a slow consumer
  // (e.g. a stalled strategy) causing unbounded memory growth.
  static constexpr size_t kMaxQueueDepth = 65536;

  // a single queued item: either a trade or an l2 snapshot. the two
  // payloads are unioned so each queue entry does not pay for a full
  // TradeEvent *and* a full 200-byte L2Snapshot at the same time.
  struct Event {
    enum class Type { TRADE, L2 } type = Type::TRADE;
    union {
      TradeEvent trade;
      L2Snapshot l2;
    };
  };

  // prop_rules carries the account's survival POLICY only (limits, warn
  // fraction, reset hour, flatten flag). The equity those limits are measured
  // against is derived from the active broker at construction -- PaperTrader's
  // seeded balance on --paper, the venue's wallet equity on --live -- so there
  // is no second copy of it that can drift from the account.
  LiveOrchestrator(EngineInstance engine,
                   std::unique_ptr<IMarketStreamer> streamer,
                   double sl_pct, double tp_pct,
                   std::string redis_host = "127.0.0.1",
                   int redis_port = 6379,
                   std::string symbol = "BTCUSDT",
                   mokuren::PropRules prop_rules = {},
                   std::string venue = "");
  ~LiveOrchestrator();

  // non-copyable / non-movable (owns threads and a unique engine).
  LiveOrchestrator(const LiveOrchestrator&) = delete;
  LiveOrchestrator& operator=(const LiveOrchestrator&) = delete;

  // start the worker thread and the streamer. returns false if already running.
  //
  // this is the ungated entry point: it arms on the first frame, which is the
  // pre-gate behaviour apart from one improvement -- every frame still goes
  // through the validator once armed, so a NaN price or a mis-sorted book stops
  // the run instead of feeding the engine.
  bool Start();

  // Start with explicit startup checks.
  //
  // The streamer is connected exactly once: a FeedRouter becomes its only
  // callback target before Start(), and holds every frame back until the gate
  // has seen enough well-formed traffic on every channel the venue profile
  // marks required. Only then is the engine armed, so the connection that
  // passed the checks is the one that trades -- there is no probe-then-
  // reconnect gap.
  //
  // Returns false if the run is already started or the checks failed;
  // PreflightReport() says which channel and why. On failure the streamer is
  // stopped and the worker joined: nothing was armed, so nothing is left to
  // unwind -- in particular the end-of-run flatten is skipped, because "close
  // at the last price" on a book the gate rejected would invent a position.
  bool Start(const mokuren::integrity::ProbeCriteria& criteria);

  // per-channel result of the startup checks. valid after Start() returns,
  // including on failure, so the caller can print it and refuse to run.
  mokuren::integrity::ProbeReport PreflightReport() const;

  // stop the streamer, drain the queue, and join the worker. idempotent.
  void Stop();

  // true while the worker thread is alive.
  bool IsRunning() const { return running_.load(std::memory_order_acquire); }

  // broker orders and state queries are routed to: the live venue adapter
  // when the config selected one, otherwise the in-process PaperTrader.
  // never null -- the paper account always exists.
  IBroker* ActiveBroker() {
    return engine_.broker ? engine_.broker.get() : engine_.ptrader.get();
  }

  // number of events dropped due to queue overflow (for monitoring).
  size_t DroppedEvents() const { return dropped_.load(std::memory_order_relaxed); }

  // true once an event was dropped for queue overflow. a dropped trade loses a
  // print; a dropped l2 snapshot leaves the book stale while triggers keep
  // being evaluated against it. new entries are refused from that point on:
  // the position stays protected by the venue-side stop, but the engine no
  // longer pretends to know the book.
  bool BookDegraded() const { return book_degraded_.load(std::memory_order_acquire); }

  // true while the streamer has signalled a book gap (reconnect, unexpected
  // close, transport error). unlike book_degraded_, this is recoverable: the
  // next real l2 snapshot clears it. new entries are refused while it is set,
  // because the local book is not a faithful picture of the venue.
  bool BookGap() const { return book_gap_.load(std::memory_order_acquire); }

  // true once the market streamer signalled a clean end of input (replay eof,
  // or the venue closed the socket) or an unrecoverable transport error. the
  // worker drains and exits; the owner should then call Stop() to flush.
  bool StreamEnded() const { return stream_ended_.load(std::memory_order_acquire); }

  // last transport error reported by the streamer; empty when there was none.
  std::string StreamErrorMessage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stream_error_;
  }

private:
  // called by the streamer callbacks; enqueues an event.
  void OnTrade(const TradeEvent& t);
  void OnL2(const L2Snapshot& s);

  // stream completion/error sinks. both run on the streamer's own thread, so
  // they must not join it: they signal the worker and return. Stop() does the
  // joining on the owner's thread.
  void OnStreamFinished();
  void OnStreamError(const std::string& message);

  // ask the worker to drain and exit. safe from any thread, including a
  // streamer callback (it never joins).
  void RequestShutdown();

  // publish a TRADE_OPEN event carrying the signal that was actually placed
  // (direction, entry, stop, target, volume). the executor has already
  // validated the stop and sized the volume by the time this runs, so these
  // are the levels the broker received -- not the raw trigger output.
  void PublishOpenEvent(const TradeSignal& signal, int64_t timestamp);

  // publish a POSITION_CLOSED event carrying the trade's pnl and exit reason.
  // those only exist in the broker's trade log, so this must be called after
  // the close has been booked. a scale-out appends one record per fragment,
  // and the pnl reported is the sum of the fragments sharing the newest
  // entry_time -- the whole trade, not just its final piece.
  void PublishCloseEvent(double price, int64_t timestamp);

  // worker thread entry point: drains the queue and feeds the engine.
  void WorkerLoop();

  // evaluate the funded-account drawdown limits against the active broker's
  // equity, and flatten once if a limit is breached. called from the worker on
  // every trade print: equity only moves when something fills, so that is the
  // natural cadence and it costs a handful of comparisons.
  void CheckPropGuard(int64_t now_ms, double price);

  // push an event; returns false if the queue is full (event dropped).
  bool Push(Event&& ev);

  // pop an event; blocks until one is available or shutdown is requested.
  // returns false if the queue was closed and drained.
  bool Pop(Event& out);

  EngineInstance engine_;
  std::unique_ptr<IMarketStreamer> streamer_;

  // owns the startup gate and validates every frame, both while probing and
  // while running. constructed in Start(), before the streamer is started, so
  // the streamer's only callback target is the router.
  std::unique_ptr<mokuren::integrity::FeedRouter> router_;

  // the shared execution funnel: stop validation, sizing, risk firewall,
  // stop re-validation and take-profit derivation. constructed in Start() so
  // it binds to the broker that is active for this run. worker-thread only.
  std::unique_ptr<mokuren::SignalExecutor> signal_executor_;

  // live bars being built from incoming trades. kept outside the
  // aggregator's history so we can mutate them without const_cast.
  Bar live_bar_{};
  Bar htf_live_bar_{};

  // risk parameters mirrored from the backtest config (cfg.sl_pct / cfg.tp_pct).
  double sl_pct_ = 0.004;
  double tp_pct_ = 0.012;

  // symbol used in broadcast events (e.g. "BTCUSDT").
  std::string symbol_;

  // venue name, used to look up the StartupGate profile. empty means "unknown",
  // which falls back to the strict profile: both channels required.
  std::string venue_;

  // funded-account survival limits. one policy, evaluated identically on the
  // paper and venue paths: an account limit is not a strategy parameter, and
  // it must not live inside a broker or a sizer where only one path sees it.
  // worker-thread only, so it needs no lock.
  mokuren::PropGuard prop_guard_;
  mokuren::PropDecision last_prop_decision_;

  // non-blocking redis pub/sub broadcaster for live events.
  std::unique_ptr<RedisPublisher> redis_publisher_;

  // bounded event queue guarded by mutex_/cv_.
  std::deque<Event> queue_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;

  std::thread worker_;
  std::atomic<bool> running_{false};
  std::atomic<bool> shutdown_{false};
  std::atomic<size_t> dropped_{0};
  std::atomic<bool> book_degraded_{false};
  std::atomic<bool> book_gap_{false};

  // set by the finished/error sinks. kept separate from shutdown_ so the
  // owner can tell "the stream ended on its own" from "we asked it to stop".
  std::atomic<bool> stream_ended_{false};
  std::string stream_error_; // guarded by mutex_
};
