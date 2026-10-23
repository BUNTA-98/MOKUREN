#include "logger.hpp"
#include "live_debug.hpp"
#include "ui_manager.hpp"
#include "live_engine.hpp"
#include "config_parser.hpp"
#include "factory.hpp"
#include "feed_router.hpp"

#include <ixwebsocket/IXNetSystem.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
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

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// --probe: connect to the venue's public feed, run it through the same
// StartupGate the live path uses, print the report and exit.
//
// The engine is never constructed, so a probe cannot arm, cannot fill and
// cannot touch the account -- which is what makes it safe to run against a
// venue at any time, including on a fresh box before keys are installed.
//
// This is what the deleted venue_doctor should have been: not a second
// implementation of "is this data good", but the same implementation with the
// trading detached. The streamer comes from the same factory live mode uses,
// so a probe cannot pass on a streamer live mode would not pick.
//
// NOTE ON DEPTH: the gate checks whatever the config asks for. A config with
// no "startup_probe" block gets Development(), whose min_l2_depth is 2, so a
// probe with defaults proves "the book is at least 2 deep", not "at least 20".
// To verify the depth the strategy actually needs, set min_l2_depth explicitly
// in the config being probed.
int RunProbe(const std::string& config_path) {
  std::signal(SIGINT, HandleSigint);

  LOG_INFO("probe: loading config from '{}'", config_path);

  std::ifstream cfg_file(config_path);
  if (!cfg_file.is_open()) {
    throw std::runtime_error("failed to open config file: " + config_path);
  }

  json cfg_json;
  cfg_file >> cfg_json;
  FlattenGrids(cfg_json);

  // a probe signs nothing: it needs the public feed and nothing else. demanding
  // credentials here would make the check useless on exactly the machines that
  // need it most.
  AppConfig cfg =
      AppConfig::Load(cfg_json, /*require_venue_credentials=*/false);

  // built exactly as live mode builds it. use_paper_broker keeps a venue
  // execution adapter out of the process; the pipeline is constructed but
  // never started, so no trigger ever sees a frame.
  EngineInstance engine = StrategyFactory::Build(
      cfg, /*live_mode=*/true, /*use_paper_broker=*/true);
  if (!engine.streamer) {
    throw std::runtime_error("probe: no market-data streamer for exchange '" +
                             cfg.exchange + "'");
  }

  const std::string venue = cfg.exchange;
  mokuren::integrity::FeedRouter router(
      venue, mokuren::integrity::ProfileForVenue(venue), cfg.probe,
      NowMillis());
  router.Attach(*engine.streamer);

  // rates, not counts: print the threshold as it will be judged, so the line
  // in the log matches the verdict instead of describing a criterion the gate
  // no longer applies.
  LOG_INFO("probe: checking {} via the public feed (trades>={}/min "
           "l2>={}/min depth>={} for >={}ms, timeout {}ms)",
           venue, cfg.probe.min_trades_per_min,
           cfg.probe.min_l2_frames_per_min, cfg.probe.min_l2_depth,
           cfg.probe.min_duration_ms, cfg.probe.timeout_ms);

  engine.streamer->Start();

  // the same cadence and progress line as LiveOrchestrator::Start: a soak must
  // not look like a hang, and ctrl-c must leave no socket behind.
  constexpr int64_t kPollIntervalMs = 20;
  constexpr int64_t kProgressIntervalMs = 5000;
  int64_t next_progress_ms = NowMillis() + kProgressIntervalMs;

  mokuren::integrity::ProbeState state = router.Poll(NowMillis());
  while (state == mokuren::integrity::ProbeState::Probing) {
    if (g_quit.load(std::memory_order_acquire)) {
      LOG_INFO("probe: interrupted");
      engine.streamer->Stop();
      return 130;  // 128 + SIGINT, the conventional shell code
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    const int64_t now = NowMillis();
    state = router.Poll(now);

    if (state == mokuren::integrity::ProbeState::Probing &&
        now >= next_progress_ms) {
      const mokuren::integrity::ProbeReport r = router.Report();
      LOG_INFO("probe: in progress ({} ms): trades={} l2={} rejected={}",
               r.elapsed_ms, r.trades.delivered, r.l2.delivered,
               r.trades.rejected() + r.l2.rejected());
      next_progress_ms = now + kProgressIntervalMs;
    }
  }

  engine.streamer->Stop();

  const mokuren::integrity::ProbeReport report = router.Report();
  std::cout << "\n" << report.ToString() << std::flush;

  if (!report.passed()) {
    LOG_ERROR("probe: {} FAILED: {}", venue, report.reason);
    std::cerr << "probe failed: " << report.reason << "\n";
    // a failed probe is an answer, not a crash: exit non-zero without throwing
    // so the report above stays the last thing on the terminal.
    return 1;
  }

  LOG_INFO("probe: {} passed in {} ms", venue, report.elapsed_ms);
  return 0;
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
  bool probe_mode = false;
  bool debug_mode = false;
  std::string config_path;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--live") == 0) {
      live_mode = true;
    } else if (std::strcmp(argv[i], "--paper") == 0) {
      paper_mode = true;
    } else if (std::strcmp(argv[i], "--probe") == 0) {
      probe_mode = true;
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
                << "' (supported: --live, --paper, --probe, --config=<path>, "
                   "--debug)\n";
      return 1;
    }
  }

  // exactly one run mode. --probe is a sibling of --live/--paper, not a flag on
  // top of them: "probe and then trade" is what --live already does (it arms
  // only once the gate passes), and "probe instead of trading" is this.
  const int modes_selected =
      (live_mode ? 1 : 0) + (paper_mode ? 1 : 0) + (probe_mode ? 1 : 0);
  if (modes_selected > 1) {
    std::cerr << "error: --live, --paper and --probe are mutually exclusive\n";
    return 1;
  }
  if (modes_selected == 1 && config_path.empty()) {
    std::cerr << "error: --live/--paper/--probe requires --config=<path> (e.g. --config=configs/btc_scalp_v1.json)\n";
    return 1;
  }

  mokuren::InitLogger("logs/engine.log",
                      debug_mode ? spdlog::level::trace : spdlog::level::info,
                      live_mode || paper_mode || probe_mode);
  // pin the global level to the same threshold. spdlog::set_level(trace) was
  // unconditional, so every release run logged at trace into the rotating
  // audit file and drowned the events that matter.
  spdlog::set_level(debug_mode ? spdlog::level::trace : spdlog::level::info);
  // record the exact invocation once. the log carries no pid and no command
  // line, so two sessions with different configs are indistinguishable once
  // they share a file -- which is how an old `test.json` launch was mistaken
  // for the run under investigation.
  {
    std::string invocation;
    for (int i = 1; i < argc; ++i) {
      if (!invocation.empty()) invocation += ' ';
      invocation += argv[i];
    }
    LOG_INFO("engine starting up (argv: {})", invocation);
  }

  int exit_code = 0;
  try {
    if (probe_mode) {
      // returns an exit code instead of throwing: a failing feed is an answer,
      // and the report it printed is the useful output, not a stack trace.
      exit_code = RunProbe(config_path);
    } else if (live_mode || paper_mode) {
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
