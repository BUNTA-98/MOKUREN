#include "logger.hpp"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <filesystem>
#include <mutex>

namespace mokuren {

namespace {
std::shared_ptr<spdlog::logger> g_logger;
std::once_flag g_init_flag;
constexpr size_t kQueueSize = 8192;
constexpr size_t kWorkerThreads = 1;
constexpr size_t kMaxFileSize = 10 * 1024 * 1024; // 10 MB
constexpr size_t kMaxFiles = 5;
} // namespace

void InitLogger(const std::string& log_path, spdlog::level::level_enum level) {
  std::call_once(g_init_flag, [&]() {
    // ensure the log directory exists
    std::filesystem::path p(log_path);
    if (p.has_parent_path()) {
      std::error_code ec;
      std::filesystem::create_directories(p.parent_path(), ec);
    }

    // dedicated async thread pool for non-blocking logging
    spdlog::init_thread_pool(kQueueSize, kWorkerThreads);

    auto console_sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        log_path, kMaxFileSize, kMaxFiles, /*rotate_on_open=*/false);

    std::vector<spdlog::sink_ptr> sinks {console_sink, file_sink};

    g_logger = std::make_shared<spdlog::async_logger>(
        "engine_audit",
        sinks.begin(),
        sinks.end(),
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);

    g_logger->set_level(level);
    g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [thread %t] %v");
    g_logger->flush_on(spdlog::level::warn);

    spdlog::register_logger(g_logger);
    spdlog::set_default_logger(g_logger);
  });
}

void ShutdownLogger() {
  if (g_logger) {
    g_logger->flush();
  }
  spdlog::shutdown();
  g_logger.reset();
}

std::shared_ptr<spdlog::logger> GetLogger() {
  return g_logger;
}

} // namespace mokuren
