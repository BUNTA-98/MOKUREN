#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <atomic>
#include <memory>
#include <mutex>

namespace mokuren {
namespace debug {

// Global switch: false by default so backtests run at full speed.
inline std::atomic<bool> g_live_debug_enabled{false};

inline std::shared_ptr<spdlog::logger>& LiveDebugLogger() {
  static std::shared_ptr<spdlog::logger> logger;
  static std::once_flag init_flag;
  std::call_once(init_flag, []() {
    try {
      logger = spdlog::basic_logger_mt("live_debug", "live_debug.log", /*truncate=*/true);
      logger->set_level(spdlog::level::trace);
      logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
      // Flush only on warn/error or periodically via spdlog to avoid SSD bottleneck on every tick
      logger->flush_on(spdlog::level::warn);
      spdlog::flush_every(std::chrono::seconds(2));
    } catch (const spdlog::spdlog_ex&) {
      logger.reset();
    }
  });
  return logger;
}

} // namespace debug
} // namespace mokuren

#define LIVE_DBG(...) do { \
  if (::mokuren::debug::g_live_debug_enabled.load(std::memory_order_relaxed)) { \
    auto& _dbg = ::mokuren::debug::LiveDebugLogger(); \
    if (_dbg) _dbg->trace(__VA_ARGS__); \
  } \
} while (0)

