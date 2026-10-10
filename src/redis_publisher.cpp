#include "redis_publisher.hpp"

#include "logger.hpp"

#include <hiredis/hiredis.h>

#include <utility>

RedisPublisher::RedisPublisher(std::string host, int port, std::string channel)
    : host_(std::move(host)), port_(port), channel_(std::move(channel)) {
  LOG_INFO("RedisPublisher: starting (host={} port={} channel={})", host_,
           port_, channel_);
  worker_ = std::thread(&RedisPublisher::WorkerLoop, this);
}

RedisPublisher::~RedisPublisher() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_.store(true, std::memory_order_release);
  }
  cv_.notify_all();

  if (worker_.joinable()) {
    worker_.join();
  }

  if (redis_ctx_) {
    redisFree(static_cast<redisContext*>(redis_ctx_));
    redis_ctx_ = nullptr;
  }

  LOG_INFO("RedisPublisher: stopped (dropped={})",
           dropped_.load(std::memory_order_relaxed));
}

bool RedisPublisher::Publish(const std::string& json_payload) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= kMaxQueueDepth) {
      size_t n = dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
      if ((n & 0x3FF) == 1) {
        LOG_WARN("RedisPublisher: queue overflow, dropped {} messages so far",
                 n);
      }
      return false;
    }
    queue_.push(json_payload);
  }
  cv_.notify_one();
  return true;
}

void RedisPublisher::Flush() {
  std::unique_lock<std::mutex> lock(mutex_);
  drained_cv_.wait(lock, [this] { return queue_.empty() && !busy_; });
}

bool RedisPublisher::EnsureConnected() {
  if (redis_ctx_) return true;

  struct timeval timeout;
  timeout.tv_sec = 1;
  timeout.tv_usec = 0;

  redisContext* ctx = redisConnectWithTimeout(host_.c_str(), port_, timeout);
  if (ctx == nullptr || ctx->err) {
    if (ctx) {
      LOG_WARN("RedisPublisher: connection error: {}", ctx->errstr);
      redisFree(ctx);
    } else {
      LOG_WARN("RedisPublisher: allocation error while connecting");
    }
    return false;
  }

  redis_ctx_ = ctx;
  LOG_INFO("RedisPublisher: connected to {}:{}", host_, port_);
  return true;
}

bool RedisPublisher::PublishRaw(const std::string& json_payload) {
  if (!EnsureConnected()) return false;

  redisContext* ctx = static_cast<redisContext*>(redis_ctx_);
  redisReply* reply = static_cast<redisReply*>(
      redisCommand(ctx, "PUBLISH %s %b", channel_.c_str(),
                   json_payload.data(), json_payload.size()));

  if (reply == nullptr) {
    LOG_WARN("RedisPublisher: publish failed, dropping connection");
    redisFree(ctx);
    redis_ctx_ = nullptr;
    return false;
  }

  freeReplyObject(reply);
  return true;
}

void RedisPublisher::WorkerLoop() {
  LOG_INFO("RedisPublisher: worker thread online");

  while (true) {
    std::string payload;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] {
        return !queue_.empty() || shutdown_.load(std::memory_order_acquire);
      });

      if (queue_.empty()) {
        // shutdown requested and queue drained.
        break;
      }

      payload = std::move(queue_.front());
      queue_.pop();
      busy_ = true;
    }

    // best-effort publish; on failure the message is dropped and the
    // connection is torn down so the next call reconnects.
    PublishRaw(payload);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      busy_ = false;
    }
    drained_cv_.notify_all();
  }

  LOG_INFO("RedisPublisher: worker thread exiting");
}
