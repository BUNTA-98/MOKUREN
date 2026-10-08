#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <memory>
#include <string>

// global async logger for the engine audit trail.
// call InitLogger() once at startup before any logging happens.
namespace mokuren {

// initializes the global async logger writing to logs/engine_audit.log.
// safe to call multiple times (subsequent calls are no-ops).
void InitLogger(const std::string& log_path = "logs/engine_audit.log",
                spdlog::level::level_enum level = spdlog::level::trace);

// flushes and shuts down the async thread pool. call before exit.
void ShutdownLogger();

// returns the global logger (may be null if InitLogger was never called).
std::shared_ptr<spdlog::logger> GetLogger();

} // namespace mokuren

// convenience macros that are safe even if the logger was never initialized.
#define LOG_TRACE(...) do { auto _lg = ::mokuren::GetLogger(); if (_lg) _lg->trace(__VA_ARGS__); } while (0)
#define LOG_DEBUG(...) do { auto _lg = ::mokuren::GetLogger(); if (_lg) _lg->debug(__VA_ARGS__); } while (0)
#define LOG_INFO(...)  do { auto _lg = ::mokuren::GetLogger(); if (_lg) _lg->info(__VA_ARGS__);  } while (0)
#define LOG_WARN(...)  do { auto _lg = ::mokuren::GetLogger(); if (_lg) _lg->warn(__VA_ARGS__);  } while (0)
#define LOG_ERROR(...) do { auto _lg = ::mokuren::GetLogger(); if (_lg) _lg->error(__VA_ARGS__); } while (0)
