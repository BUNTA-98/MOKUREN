#include "redis_publisher.hpp"

#include "logger.hpp"

#include <hiredis/hiredis.h>
#include <spdlog/fmt/fmt.h>

#include <chrono>
#include <utility>
#include <vector>

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

bool RedisPublisher::EnqueueLocked(OutboundMessage&& msg) {
  if (queue_.size() >= kMaxQueueDepth) {
    size_t n = dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & 0x3FF) == 1) {
      LOG_WARN("RedisPublisher: queue overflow, dropped {} messages so far", n);
    }
    return false;
  }
  queue_.push(std::move(msg));
  return true;
}

bool RedisPublisher::Publish(const std::string& json_payload) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    OutboundMessage msg;
    msg.kind = OutboundMessage::Kind::PubSub;
    msg.target = channel_;
    msg.payload = json_payload;
    if (!EnqueueLocked(std::move(msg))) {
      return false;
    }
  }
  cv_.notify_one();
  return true;
}

bool RedisPublisher::PublishTradeEvent(const std::string& json_payload) {
  bool ok = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1) existing pub/sub path (telegram bot) - unchanged semantics.
    OutboundMessage pubsub;
    pubsub.kind = OutboundMessage::Kind::PubSub;
    pubsub.target = channel_;
    pubsub.payload = json_payload;
    ok = EnqueueLocked(std::move(pubsub)) && ok;

    // 2) persistent stream path for grafana (same exact json payload).
    OutboundMessage xadd;
    xadd.kind = OutboundMessage::Kind::XAdd;
    xadd.target = kTradeStream;
    xadd.fields.emplace_back("payload", json_payload);
    ok = EnqueueLocked(std::move(xadd)) && ok;
  }
  cv_.notify_all();
  return ok;
}

bool RedisPublisher::PublishTicker(const std::string& symbol, double price,
                                   int64_t timestamp_ms) {
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(mutex_);

    // throttle: at most one ticker write per kTickerThrottleMs.
    if (last_ticker_time_ != std::chrono::steady_clock::time_point{} &&
        now - last_ticker_time_ <
            std::chrono::milliseconds(kTickerThrottleMs)) {
      return false;
    }
    last_ticker_time_ = now;

    OutboundMessage msg;
    msg.kind = OutboundMessage::Kind::XAdd;
    msg.target = kTickerStream;
    msg.fields.emplace_back("symbol", symbol);
    msg.fields.emplace_back("price", fmt::format("{:.8f}", price));
    msg.fields.emplace_back("timestamp", std::to_string(timestamp_ms));
    if (!EnqueueLocked(std::move(msg))) {
      return false;
    }
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

bool RedisPublisher::XAddRaw(
    const std::string& stream,
    const std::vector<std::pair<std::string, std::string>>& fields) {
  if (fields.empty()) return false;
  if (!EnsureConnected()) return false;

  redisContext* ctx = static_cast<redisContext*>(redis_ctx_);

  // build argv manually so payloads are never interpreted as format strings:
  //   XADD <stream> * <field> <value> ...
  static const char kCmd[] = "XADD";
  static const char kAutoId[] = "*";

  std::vector<const char*> argv;
  std::vector<size_t> argvlen;
  argv.reserve(3 + fields.size() * 2);
  argvlen.reserve(3 + fields.size() * 2);

  argv.push_back(kCmd);
  argvlen.push_back(sizeof(kCmd) - 1);
  argv.push_back(stream.c_str());
  argvlen.push_back(stream.size());
  argv.push_back(kAutoId);
  argvlen.push_back(sizeof(kAutoId) - 1);
  for (const auto& field : fields) {
    argv.push_back(field.first.c_str());
    argvlen.push_back(field.first.size());
    argv.push_back(field.second.c_str());
    argvlen.push_back(field.second.size());
  }

  redisReply* reply = static_cast<redisReply*>(redisCommandArgv(
      ctx, static_cast<int>(argv.size()), argv.data(), argvlen.data()));

  if (reply == nullptr) {
    LOG_WARN("RedisPublisher: XADD failed, dropping connection");
    redisFree(ctx);
    redis_ctx_ = nullptr;
    return false;
  }

  const bool ok = (reply->type != REDIS_REPLY_ERROR);
  if (!ok) {
    LOG_WARN("RedisPublisher: XADD error: {}",
             reply->str ? reply->str : "unknown");
  }
  freeReplyObject(reply);
  return ok;
}

void RedisPublisher::WorkerLoop() {
  LOG_INFO("RedisPublisher: worker thread online");

  while (true) {
    OutboundMessage msg;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] {
        return !queue_.empty() || shutdown_.load(std::memory_order_acquire);
      });

      if (queue_.empty()) {
        // shutdown requested and queue drained.
        break;
      }

      msg = std::move(queue_.front());
      queue_.pop();
      busy_ = true;
    }

    // best-effort delivery; on failure the message is dropped and the
    // connection is torn down so the next call reconnects.
    if (msg.kind == OutboundMessage::Kind::PubSub) {
      PublishRaw(msg.payload);
    } else {
      XAddRaw(msg.target, msg.fields);
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      busy_ = false;
    }
    drained_cv_.notify_all();
  }

  LOG_INFO("RedisPublisher: worker thread exiting");
}
