#include "logger.hpp"

#include "log_ring.hpp"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <stdexcept>

namespace mokuren {

namespace {
std::shared_ptr<spdlog::logger> g_logger;
std::shared_ptr<spdlog::sinks::sink> g_ring_sink;
spdlog::level::level_enum g_file_level = spdlog::level::info;
// std::once_flag is not assignable, so it cannot be reset after
// spdlog::shutdown(). an atomic flag gives the same "init once" guarantee
// while still allowing a later InitLogger() to rebuild the thread pool.
std::atomic<bool> g_initialized{false};
constexpr size_t kQueueSize = 8192;
// 2, not 1: the single-worker configuration could not keep up with a grid
// scan at debug across N worker threads, and overrun_oldest silently drops
// the oldest line before the sink formats it. a log that silently drops
// lines is worse than no log at that level. ordering across two workers is
// not guaranteed, but LogRing::Push assigns seq under a mutex, so each
// entry is atomic and the panel sees a coherent stream.
constexpr size_t kWorkerThreads = 2;
constexpr size_t kMaxFileSize = 10 * 1024 * 1024; // 10 MB
constexpr size_t kMaxFiles = 5;

class LogRingSink : public spdlog::sinks::base_sink<std::mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t buf;
        formatter_->format(msg, buf);
        std::string s(buf.data(), buf.size());
        for (auto& c : s) if (c == '\n' || c == '\r') c = ' ';
        mokuren::GetLogRing().Push(msg.level, std::move(s));
    }
    void flush_() override {}
};
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
    auto ring_sink = std::make_shared<LogRingSink>();

    std::vector<spdlog::sink_ptr> sinks {file_sink, ring_sink};

    std::shared_ptr<spdlog::sinks::stdout_sink_mt> console_sink;
    if (enable_console) {
      console_sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
      sinks.push_back(console_sink);
    }

    g_logger = std::make_shared<spdlog::async_logger>(
        "engine_audit",
        sinks.begin(),
        sinks.end(),
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);

    // the logger gate is the *min* of every sink's level. it used to sit at
    // trace, which defeated short-circuiting: every LOG_TRACE in the
    // trigger/aggregator hot path formatted and enqueued into the async
    // pool even though both sinks discarded it. the gate now matches the
    // most verbose sink, so a trace macro above that level costs one
    // should_log() call and nothing else.
    g_file_level = level;
    g_logger->set_level(level);
    file_sink->set_level(level);
    if (console_sink) console_sink->set_level(level);
    // the ring follows the file by default. backtests stay at info: cheap,
    // bounded, and the whole sweep fits in memory. SetLogRingLevel() raises
    // this when the operator actually opens the panel, and the async pool
    // pays debug cost only while somebody is looking at it.
    ring_sink->set_level(level);
    g_ring_sink = ring_sink;

    // pid in the pattern: without it a TUI session and the kaito daemon
    // writing to the same file were indistinguishable, which is how an old
    // `test.json` launch was mistaken for the run under investigation.
    g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [pid %P] [thread %t] %v");
    // re-apply a shorter pattern on the ring only. the line above rewrites
    // every sink's pattern, and a 50-char prefix per line is unusable in the
    // panel body.
    ring_sink->set_pattern("[%H:%M:%S] [%l] %v");
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
  g_ring_sink.reset();
  g_file_level = spdlog::level::info;
  g_logger.reset();
  // spdlog::shutdown() tears down the thread pool, so a later InitLogger()
  // must be allowed to rebuild it. leaving the flag set made the second
  // call a no-op and GetLogger() returned a logger whose pool was gone.
  g_initialized.store(false);
}

std::shared_ptr<spdlog::logger> GetLogger() {
  return g_logger;
}

void SetLogRingLevel(spdlog::level::level_enum lvl) {
  if (!g_logger || !g_ring_sink) return;
  g_ring_sink->set_level(lvl);
  // the logger gate must be at least as verbose as the most verbose sink,
  // or the line is filtered before either sink sees it.
  const spdlog::level::level_enum gate =
      (g_file_level < lvl) ? g_file_level : lvl;
  g_logger->set_level(gate);
}

} // namespace mokuren
