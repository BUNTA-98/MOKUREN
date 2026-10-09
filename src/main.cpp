#include "logger.hpp"
#include "ui_manager.hpp"
#include "binance_streamer.hpp"
#include "live_engine.hpp"

#include <ixwebsocket/IXNetSystem.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>

// set by the SIGINT handler; polled by the live loop for graceful shutdown.
std::atomic<bool> g_quit{false};

namespace {

void HandleSigint(int /*signum*/) {
  g_quit.store(true, std::memory_order_release);
}

// launches the interactive TUI (grid scan / WFA / replay).
void RunBacktest() {
  try {
    UIManager ui;
    ui.Run();
  } catch (const std::exception& e) {
    LOG_ERROR("fataler fehler: {}", e.what());
    std::cerr << "fataler fehler: " << e.what() << "\n";
    throw;
  }
}

// headless live forward-test: streams binance data into the orchestrator.
// the engine instance is intentionally left empty for now so we can
// verify the websocket data flow via TRACE logs without running the
// full strategy pipeline.
void RunLive() {
  std::signal(SIGINT, HandleSigint);

  LOG_INFO("live mode: connecting to binance streamer");

  EngineInstance engine; // dummy: all pointers null, WorkerLoop null-checks them
  auto streamer = std::make_unique<BinanceDataStreamer>();

  LiveOrchestrator orchestrator(std::move(engine), std::move(streamer), 0.01, 0.01);

  if (!orchestrator.Start()) {
    LOG_ERROR("live mode: failed to start orchestrator");
    return;
  }

  LOG_INFO("live mode: streaming (press ctrl-c to stop)");

  while (!g_quit.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  LOG_INFO("live mode: shutdown requested, stopping orchestrator");
  orchestrator.Stop();
  LOG_INFO("live mode: stopped cleanly (dropped events: {})", orchestrator.DroppedEvents());
}

} // namespace

int main(int argc, char** argv) {
  ix::initNetSystem();

  mokuren::InitLogger();
  spdlog::set_level(spdlog::level::trace);
  LOG_INFO("engine starting up");

  bool live_mode = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--live") == 0) {
      live_mode = true;
    }
  }

  int exit_code = 0;
  try {
    if (live_mode) {
      RunLive();
    } else {
      RunBacktest();
    }
  } catch (const std::exception& e) {
    LOG_ERROR("fataler fehler: {}", e.what());
    std::cerr << "fataler fehler: " << e.what() << "\n";
    exit_code = 1;
  }

  LOG_INFO("engine shutting down");
  mokuren::ShutdownLogger();
  return exit_code;
}
