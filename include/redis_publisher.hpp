#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// forwards a stream of JSON strings to a redis pub/sub channel.
//
// design goals:
//   - strictly non-blocking on the caller side: Publish() only enqueues.
//   - a single background worker owns the redis connection and does the
//     actual (blocking) network i/o.
//   - bounded queue: if the consumer cannot keep up, events are dropped
//     rather than stalling the trading loop.
//   - clean shutdown: the destructor drains the queue and joins the worker.
class RedisPublisher {
public:
  // hard cap on queued messages. protects the trading loop from a slow
  // or unreachable redis instance.
  static constexpr size_t kMaxQueueDepth = 8192;

  RedisPublisher(std::string host, int port, std::string channel);
  ~RedisPublisher();

  // non-copyable / non-movable (owns a thread and a connection).
  RedisPublisher(const RedisPublisher&) = delete;
  RedisPublisher& operator=(const RedisPublisher&) = delete;

  // enqueue a json payload for publication. never blocks.
  // returns false if the queue was full and the message was dropped.
  bool Publish(const std::string& json_payload);

  // number of messages dropped due to queue overflow.
  size_t DroppedMessages() const {
    return dropped_.load(std::memory_order_relaxed);
  }

  // block until the internal queue is empty and all pending messages
  // have been handed off to the redis connection. intended for use
  // during shutdown so that final events (e.g. SYSTEM_SHUTDOWN) are
  // not lost when the publisher is destroyed.
  void Flush();

  // enqueue the trade event json for BOTH the pub/sub channel (the existing
  // "mokuren:events" PUBLISH) and the persistent "mokuren:trades" stream
  // (XADD). never blocks on network i/o. returns false if either enqueue
  // was dropped due to queue overflow.
  bool PublishTradeEvent(const std::string& json_payload);

  // append a single price tick to the "mokuren:ticker" stream (XADD).
  // throttled to at most one write per kTickerThrottleMs. never blocks;
  // returns false if the tick was throttled or dropped.
  bool PublishTicker(const std::string& symbol, double price,
                     int64_t timestamp_ms);

  // redis stream keys used for grafana persistence.
  static constexpr const char* kTradeStream = "mokuren:trades";
  static constexpr const char* kTickerStream = "mokuren:ticker";
  // minimum spacing between two ticker writes.
  static constexpr int64_t kTickerThrottleMs = 1000;

private:
  // worker thread entry point: connects, then drains the queue.
  void WorkerLoop();

  // (re)establish the redis connection. returns true on success.
  bool EnsureConnected();

  // a single unit of work for the background worker thread.
  struct OutboundMessage {
    enum class Kind { PubSub, XAdd };
    Kind kind = Kind::PubSub;
    std::string target;  // pub/sub channel (PubSub) or stream key (XAdd)
    std::string payload; // pub/sub payload (unused for XAdd)
    // field/value pairs for XAdd messages.
    std::vector<std::pair<std::string, std::string>> fields;
  };

  // push a message onto the bounded queue. caller must hold mutex_.
  // returns false if the queue was full and the message was dropped.
  bool EnqueueLocked(OutboundMessage&& msg);

  // publish a single payload to the pub/sub channel.
  bool PublishRaw(const std::string& json_payload);

  // append a single entry to a stream. returns false on connection error.
  bool XAddRaw(const std::string& stream,
               const std::vector<std::pair<std::string, std::string>>& fields);

  std::string host_;
  int port_;
  std::string channel_;

  // opaque redis context pointer (redisContext*). kept as void* so the
  // header does not need to pull in hiredis.
  void* redis_ctx_ = nullptr;

  std::queue<OutboundMessage> queue_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;

  // timestamp of the last accepted ticker write (guarded by mutex_).
  std::chrono::steady_clock::time_point last_ticker_time_{};

  // signalled by the worker after it finishes publishing an item.
  // used by Flush() to wait until the queue is fully drained.
  std::condition_variable drained_cv_;
  bool busy_ = false;

  std::thread worker_;
  std::atomic<bool> shutdown_{false};
  std::atomic<size_t> dropped_{0};
};
