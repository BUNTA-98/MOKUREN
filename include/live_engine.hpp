#pragma once
#include "aggregator.hpp"
#include "factory.hpp"
#include "ibroker.hpp"
#include "live_streamer.hpp"
#include "logger.hpp"
#include "redis_publisher.hpp"
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

  LiveOrchestrator(EngineInstance engine,
                   std::unique_ptr<IMarketStreamer> streamer,
                   double sl_pct, double tp_pct,
                   std::string redis_host = "127.0.0.1",
                   int redis_port = 6379,
                   std::string symbol = "BTCUSDT");
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

  // broker orders and state queries are routed to: the live venue adapter
  // when the config selected one, otherwise the in-process PaperTrader.
  // never null -- the paper account always exists.
  IBroker* ActiveBroker() {
    return engine_.broker ? engine_.broker.get() : engine_.ptrader.get();
  }

  // number of events dropped due to queue overflow (for monitoring).
  size_t DroppedEvents() const { return dropped_.load(std::memory_order_relaxed); }

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

  // publish a structured json event to the redis pub/sub channel.
  // action is e.g. "TRADE_OPEN" or "POSITION_CLOSED".
  void PublishEvent(const char* action, double price, double size,
                    int64_t timestamp);

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

  // symbol used in broadcast events (e.g. "BTCUSDT").
  std::string symbol_;

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

  // set by the finished/error sinks. kept separate from shutdown_ so the
  // owner can tell "the stream ended on its own" from "we asked it to stop".
  std::atomic<bool> stream_ended_{false};
  std::string stream_error_; // guarded by mutex_
};
