#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

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

private:
  // worker thread entry point: connects, then drains the queue.
  void WorkerLoop();

  // (re)establish the redis connection. returns true on success.
  bool EnsureConnected();

  // publish a single payload. returns false on connection error.
  bool PublishRaw(const std::string& json_payload);

  std::string host_;
  int port_;
  std::string channel_;

  // opaque redis context pointer (redisContext*). kept as void* so the
  // header does not need to pull in hiredis.
  void* redis_ctx_ = nullptr;

  std::queue<std::string> queue_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;

  // signalled by the worker after it finishes publishing an item.
  // used by Flush() to wait until the queue is fully drained.
  std::condition_variable drained_cv_;
  bool busy_ = false;

  std::thread worker_;
  std::atomic<bool> shutdown_{false};
  std::atomic<size_t> dropped_{0};
};
