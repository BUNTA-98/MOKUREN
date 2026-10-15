#include "logger.hpp"
#include "live_debug.hpp"
#include "ui_manager.hpp"
#include "binance_streamer.hpp"
#include "bybit_streamer.hpp"
#include "live_engine.hpp"
#include "config_parser.hpp"
#include "factory.hpp"

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

// headless live forward-test: streams exchange data into the orchestrator.
// loads the strategy pipeline from a json config file so websocket events
// flow through Aggregator -> AlphaEngine -> PaperTrader instead of being dropped.
void RunLive(const std::string& config_path) {
  std::signal(SIGINT, HandleSigint);

  LOG_INFO("live mode: loading config from '{}'", config_path);

  std::ifstream cfg_file(config_path);
  if (!cfg_file.is_open()) {
    throw std::runtime_error("failed to open config file: " + config_path);
  }

  json cfg_json;
  cfg_file >> cfg_json;

  // collapse grid definitions ({"min","max","step","val"}) into their scalar
  // "val" so the factory only sees plain numbers. the backtest path keeps the
  // original grid objects for GridScanner.
  FlattenGrids(cfg_json);

  AppConfig cfg = AppConfig::Load(cfg_json);
  EngineInstance engine = StrategyFactory::Build(cfg);

  // pick the data source; both derive from IMarketStreamer so the
  // orchestrator stays exchange-agnostic.
  std::unique_ptr<IMarketStreamer> streamer;
  if (cfg.exchange == "bybit") {
    streamer = std::make_unique<BybitDataStreamer>();
  } else {
    streamer = std::make_unique<BinanceDataStreamer>();
  }

  LiveOrchestrator orchestrator(std::move(engine), std::move(streamer), 0.01, 0.01);

  if (!orchestrator.Start()) {
    LOG_ERROR("live mode: failed to start orchestrator");
    return;
  }

  LOG_INFO("live mode: streaming via '{}' (press ctrl-c to stop)", cfg.exchange);
  LIVE_DBG("main: live loop entered, orchestrator started");

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

  bool live_mode = false;
  std::string config_path;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--live") == 0) {
      live_mode = true;
    } else if (std::strncmp(argv[i], "--config=", 9) == 0) {
      config_path = argv[i] + 9;
    } else if (std::strcmp(argv[i], "--debug") == 0) {
      mokuren::debug::g_live_debug_enabled.store(true, std::memory_order_relaxed);
    }
  }

  if (live_mode && config_path.empty()) {
    std::cerr << "error: --live requires --config=<path> (e.g. --config=configs/btc_scalp_v1.json)\n";
    return 1;
  }

  mokuren::InitLogger("logs/engine.log", spdlog::level::info, live_mode);
  spdlog::set_level(spdlog::level::trace);
  LOG_INFO("engine starting up");

  int exit_code = 0;
  try {
    if (live_mode) {
      RunLive(config_path);
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
