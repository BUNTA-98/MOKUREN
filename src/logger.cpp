#include "logger.hpp"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <stdexcept>

namespace mokuren {

namespace {
std::shared_ptr<spdlog::logger> g_logger;
// std::once_flag is not assignable, so it cannot be reset after
// spdlog::shutdown(). an atomic flag gives the same "init once" guarantee
// while still allowing a later InitLogger() to rebuild the thread pool.
std::atomic<bool> g_initialized{false};
constexpr size_t kQueueSize = 8192;
constexpr size_t kWorkerThreads = 1;
constexpr size_t kMaxFileSize = 10 * 1024 * 1024; // 10 MB
constexpr size_t kMaxFiles = 5;
} // namespace

void InitLogger(const std::string& log_path, spdlog::level::level_enum level, bool enable_console) {
  bool expected = false;
  if (!g_initialized.compare_exchange_strong(expected, true)) {
    return;
  }

  // the flag is latched before the sink is built, so a failed open used to
  // leave it set: every later InitLogger() was a silent no-op and GetLogger()
  // returned null, which is why "check the engine log" could point at a file
  // nobody ever wrote. clear it on any failure so a retry can succeed.
  try {
    // ensure the log directory exists
    std::filesystem::path p(log_path);
    if (p.has_parent_path()) {
      std::error_code ec;
      std::filesystem::create_directories(p.parent_path(), ec);
      if (ec) {
        throw std::runtime_error("cannot create log directory '" +
                                 p.parent_path().string() + "': " + ec.message());
      }
    }

    // dedicated async thread pool for non-blocking logging
    spdlog::init_thread_pool(kQueueSize, kWorkerThreads);

    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        log_path, kMaxFileSize, kMaxFiles, /*rotate_on_open=*/false);

    std::vector<spdlog::sink_ptr> sinks {file_sink};
    if (enable_console) {
      sinks.push_back(std::make_shared<spdlog::sinks::stdout_sink_mt>());
    }

    g_logger = std::make_shared<spdlog::async_logger>(
        "engine_audit",
        sinks.begin(),
        sinks.end(),
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);

    g_logger->set_level(level);
    // pid in the pattern: without it a TUI session and the kaito daemon
    // writing to the same file were indistinguishable, which is how an old
    // `test.json` launch was mistaken for the run under investigation.
    g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [pid %P] [thread %t] %v");
    g_logger->flush_on(spdlog::level::warn);

    spdlog::register_logger(g_logger);
    spdlog::set_default_logger(g_logger);
  } catch (...) {
    // a failed init must not make every later call a silent no-op
    g_initialized.store(false);
    throw;
  }
}

void ShutdownLogger() {
  if (g_logger) {
    g_logger->flush();
  }
  spdlog::shutdown();
  g_logger.reset();
  // spdlog::shutdown() tears down the thread pool, so a later InitLogger()
  // must be allowed to rebuild it. leaving the flag set made the second
  // call a no-op and GetLogger() returned a logger whose pool was gone.
  g_initialized.store(false);
}

std::shared_ptr<spdlog::logger> GetLogger() {
  return g_logger;
}

} // namespace mokuren
