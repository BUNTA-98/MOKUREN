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
//
// use_paper_broker selects shadow mode (--paper): the real venue websocket is
// still used, but no execution adapter is constructed, so every signal fills in
// the in-process PaperTrader instead of leaving the process.
void RunLive(const std::string& config_path, bool use_paper_broker) {
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

  // a shadow run never signs a request, so it must not be forced to carry
  // venue credentials just to load its config. every other caller uses the
  // default (true) and keeps the strict check.
  AppConfig cfg = AppConfig::Load(cfg_json,
                                  /*require_venue_credentials=*/!use_paper_broker);

  // shadow mode: the paper account is the account, so a prop run that does not
  // name its own starting equity measures drawdown against the paper seed
  // instead of deriving it from the first observation (which arrives after the
  // first fill and therefore understates the day's start).
  if (use_paper_broker && cfg.prop.initial_balance <= 0.0) {
    cfg.prop.initial_balance = cfg.initial_balance;
  }

  // live_mode=true is what lets the factory construct a venue adapter. the
  // backtest/WFA/replay paths keep the default (false) and can never dial out.
  // use_paper_broker keeps the streamer but bypasses the adapter (shadow mode).
  EngineInstance engine = StrategyFactory::Build(cfg, /*live_mode=*/true,
                                                 /*use_paper_broker=*/use_paper_broker);

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
  // the venue name selects the data-integrity profile (which channels are
  // required) and labels the startup report. leaving it at the ctor default
  // made every run look like an unknown venue: the strict profile still
  // applies, but the report could not say which venue passed or which one got
  // checked, and the per-venue streamer is NOT re-derived here -- it comes
  // from the factory, above.
  LiveOrchestrator orchestrator(std::move(engine), std::move(streamer),
                                cfg.sl_pct, cfg.tp_pct,
                                /*redis_host=*/"127.0.0.1", /*redis_port=*/6379,
                                cfg.symbol,
                                /*prop_rules=*/cfg.prop,
                                /*venue=*/cfg.exchange);

  // --- startup data-integrity gate -----------------------------------------
  // the engine is not armed until every required channel has produced
  // well-formed frames for the configured soak. that same connection then
  // trades, so a venue which passes the checks cannot behave differently on a
  // second handshake -- which is exactly what a separate probe process gave us.
  if (!orchestrator.Start(cfg.probe)) {
    const mokuren::integrity::ProbeReport report =
        orchestrator.PreflightReport();
    std::cout << "\n" << report.ToString();
    LOG_ERROR("live mode: startup data-integrity checks failed: {}",
              report.reason);

    if (cfg.probe_abort_on_failure) {
      // fail closed. a feed that cannot produce a clean book is not a feed
      // worth trading, and a half-blind engine is worse than no engine.
      throw std::runtime_error(
          "live mode: refusing to trade on a feed that failed its startup "
          "checks (" + report.reason + ")");
    }

    // on_failure=warn is an operator override for a venue being debugged. it
    // reconnects without the gate, which is precisely the gap the gate exists
    // to remove -- hence opt-in, logged, and never the default.
    LOG_WARN("live mode: startup_probe.on_failure=warn -- starting WITHOUT the "
             "startup checks (a second handshake, ungated)");
    if (!orchestrator.Start()) {
      throw std::runtime_error("live mode: failed to start orchestrator");
    }
  } else {
    // the "ok / ok" table: one row per channel plus the verdict.
    std::cout << orchestrator.PreflightReport().ToString();
  }

  LOG_INFO("live mode: streaming via '{}' in {} execution (press ctrl-c to stop)",
           cfg.exchange, use_paper_broker ? "paper (shadow)" : "venue");
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
  bool paper_mode = false;
  bool debug_mode = false;
  std::string config_path;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--live") == 0) {
      live_mode = true;
    } else if (std::strcmp(argv[i], "--paper") == 0) {
      paper_mode = true;
    } else if (std::strncmp(argv[i], "--config=", 9) == 0) {
      config_path = argv[i] + 9;
    } else if (std::strcmp(argv[i], "--debug") == 0) {
      debug_mode = true;
      mokuren::debug::g_live_debug_enabled.store(true, std::memory_order_relaxed);
    } else {
      // an unrecognised flag used to be ignored, which silently fell through to
      // the interactive TUI: a typo like "--paepr" and a stale binary that
      // predates the flag both looked like "it launched the backtest instead".
      // the flag set is small and closed, so refuse anything else.
      std::cerr << "error: unknown argument '" << argv[i]
                << "' (supported: --live, --paper, --config=<path>, --debug)\n";
      return 1;
    }
  }

  if (live_mode && paper_mode) {
    std::cerr << "error: --live and --paper are mutually exclusive\n";
    return 1;
  }
  if ((live_mode || paper_mode) && config_path.empty()) {
    std::cerr << "error: --live/--paper requires --config=<path> (e.g. --config=configs/btc_scalp_v1.json)\n";
    return 1;
  }

  mokuren::InitLogger("logs/engine.log",
                      debug_mode ? spdlog::level::trace : spdlog::level::info,
                      live_mode || paper_mode);
  // pin the global level to the same threshold. spdlog::set_level(trace) was
  // unconditional, so every release run logged at trace into the rotating
  // audit file and drowned the events that matter.
  spdlog::set_level(debug_mode ? spdlog::level::trace : spdlog::level::info);
  LOG_INFO("engine starting up");

  int exit_code = 0;
  try {
    if (live_mode || paper_mode) {
      RunLive(config_path, /*use_paper_broker=*/paper_mode);
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
