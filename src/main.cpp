#include "logger.hpp"
#include "live_debug.hpp"
#include "ui_manager.hpp"
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

  // live_mode=true is what lets the factory construct a venue adapter. the
  // backtest/WFA/replay paths keep the default (false) and can never dial out.
  EngineInstance engine = StrategyFactory::Build(cfg, /*live_mode=*/true);

  // the streamer is built by the factory from the same resolved venue spec as
  // the broker. deciding it here, independently, is how a bybit config ended
  // up streaming mainnet data into a testnet broker.
  if (!engine.streamer) {
    throw std::runtime_error("live mode: no market-data streamer for exchange '" +
                             cfg.exchange + "'");
  }
  auto streamer = std::move(engine.streamer);

  // risk parameters come from the config. the old literal 1%/1% silently
  // overrode every tuned backtest. the symbol comes from the same venue spec
  // the broker and streamer were built from: leaving it at the ctor default
  // made every redis event name BTCUSDT regardless of the instrument traded.
  // the account's survival rules travel with the config, not with the broker:
  // the guard must behave the same whether orders go to the paper account or
  // to a funded venue.
  LiveOrchestrator orchestrator(std::move(engine), std::move(streamer),
                                cfg.sl_pct, cfg.tp_pct,
                                /*redis_host=*/"127.0.0.1", /*redis_port=*/6379,
                                cfg.symbol,
                                /*prop_rules=*/cfg.prop);

  if (!orchestrator.Start()) {
    LOG_ERROR("live mode: failed to start orchestrator");
    return;
  }

  LOG_INFO("live mode: streaming via '{}' (press ctrl-c to stop)", cfg.exchange);
  LIVE_DBG("main: live loop entered, orchestrator started");

  while (!g_quit.load(std::memory_order_acquire)) {
    // a clean end of stream or a dropped socket ends the run by itself.
    // without this the loop slept forever and the failure looked exactly like
    // a quiet market.
    if (orchestrator.StreamEnded()) {
      const std::string err = orchestrator.StreamErrorMessage();
      if (err.empty()) {
        LOG_INFO("live mode: market stream ended");
      } else {
        LOG_ERROR("live mode: market stream aborted: {}", err);
      }
      break;
    }
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
  bool debug_mode = false;
  std::string config_path;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--live") == 0) {
      live_mode = true;
    } else if (std::strncmp(argv[i], "--config=", 9) == 0) {
      config_path = argv[i] + 9;
    } else if (std::strcmp(argv[i], "--debug") == 0) {
      debug_mode = true;
      mokuren::debug::g_live_debug_enabled.store(true, std::memory_order_relaxed);
    }
  }

  if (live_mode && config_path.empty()) {
    std::cerr << "error: --live requires --config=<path> (e.g. --config=configs/btc_scalp_v1.json)\n";
    return 1;
  }

  mokuren::InitLogger("logs/engine.log",
                      debug_mode ? spdlog::level::trace : spdlog::level::info,
                      live_mode);
  // pin the global level to the same threshold. spdlog::set_level(trace) was
  // unconditional, so every release run logged at trace into the rotating
  // audit file and drowned the events that matter.
  spdlog::set_level(debug_mode ? spdlog::level::trace : spdlog::level::info);
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
