#pragma once
#include "aggregator.hpp"
#include "factory.hpp"
#include "live_streamer.hpp"
#include "logger.hpp"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
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

  // a single queued item: either a trade or an l2 snapshot.
  struct Event {
    enum class Type { TRADE, L2 } type;
    TradeEvent trade{};
    L2Snapshot l2{};
  };

  LiveOrchestrator(EngineInstance engine,
                   std::unique_ptr<IMarketStreamer> streamer,
                   double sl_pct, double tp_pct);
  ~LiveOrchestrator();

  // non-copyable / non-movable (owns threads and a unique engine).
  LiveOrchestrator(const LiveOrchestrator&) = delete;
  LiveOrchestrator& operator=(const LiveOrchestrator&) = delete;

  // start the worker thread and the streamer. returns false if already running.
  bool Start();

  // stop the streamer, drain the queue, and join the worker. idempotent.
  void Stop();

  // true while the worker thread is alive.
  bool IsRunning() const { return running_.load(std::memory_order_acquire); }

  // number of events dropped due to queue overflow (for monitoring).
  size_t DroppedEvents() const { return dropped_.load(std::memory_order_relaxed); }

private:
  // called by the streamer callbacks; enqueues an event.
  void OnTrade(const TradeEvent& t);
  void OnL2(const L2Snapshot& s);

  // worker thread entry point: drains the queue and feeds the engine.
  void WorkerLoop();

  // push an event; returns false if the queue is full (event dropped).
  bool Push(Event&& ev);

  // pop an event; blocks until one is available or shutdown is requested.
  // returns false if the queue was closed and drained.
  bool Pop(Event& out);

  EngineInstance engine_;
  std::unique_ptr<IMarketStreamer> streamer_;

  // live bars being built from incoming trades. kept outside the
  // aggregator's history so we can mutate them without const_cast.
  Bar live_bar_{};
  Bar htf_live_bar_{};

  // risk parameters mirrored from the backtest config (cfg.sl_pct / cfg.tp_pct).
  double sl_pct_ = 0.004;
  double tp_pct_ = 0.012;

  // bounded event queue guarded by mutex_/cv_.
  std::deque<Event> queue_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;

  std::thread worker_;
  std::atomic<bool> running_{false};
  std::atomic<bool> shutdown_{false};
  std::atomic<size_t> dropped_{0};
};
