#pragma once
#include "aggregator.hpp"
#include "alpha_engine.hpp"
#include "config_parser.hpp"
#include "filter.hpp"
#include "ibroker.hpp"
#include "live_streamer.hpp"
#include "papertrader.hpp"
#include "position_manager.hpp"
#include "position_sizer.hpp"
#include "risk_engine.hpp"
#include "strategy.hpp"
#include <memory>
#include <vector>

// holds all instantiated modules for a single strategy run
struct EngineInstance {
  std::unique_ptr<Aggregator> aggregator;
  std::unique_ptr<Aggregator> htf_aggregator;
  std::unique_ptr<PaperTrader> ptrader;
  // live execution venue. null on the paper/backtest path; set to a BybitBroker
  // when the config selects exchange "bybit" and the run is live.
  // when set, LiveOrchestrator::ActiveBroker() and every module that takes an
  // IBroker* (PositionManager, PositionSizer, the risk firewall) are bound to
  // it instead of ptrader -- ptrader remains constructed but is bypassed.
  std::unique_ptr<IBroker> broker;

  // live market-data source, built from the same venue spec as broker. kept
  // here so the two can only ever be constructed together: deciding them in
  // different places is how a config streamed mainnet data into a testnet
  // account. null on the backtest path.
  std::unique_ptr<IMarketStreamer> streamer;

  std::unique_ptr<PositionManager> pos_manager;
  std::unique_ptr<PositionSizer> sizer;

  std::unique_ptr<ITrigger> trigger;
  std::vector<std::unique_ptr<ITrigger>> sub_triggers; 
  
  std::vector<std::unique_ptr<IFilter>> filters;
  std::unique_ptr<PipelineStrategy> pipeline;
  std::unique_ptr<AlphaEngine> alpha;

  std::vector<std::unique_ptr<IRiskModule>> risk_modules;
  std::unique_ptr<RiskManager> risk_manager;
};

// parses json config and wires all components together.
//
// live_mode gates the execution venue: a venue adapter is only constructed
// for a live run. a backtest or WFA must never dial out to an exchange, even
// when its config names one -- the adapter trades nothing until its state
// feed is populated, so the run looked like a strategy that found no setups.
//
// throws std::runtime_error when the config names a trigger, filter or risk
// module this build does not implement, or when no trigger resolves at all.
//
// Build is also the single place that arms trade management: it disables the
// PaperTrader ladder and configures the PositionManager from cfg.tm_config, so
// a live run and a backtest share one management owner. the tm_config levels
// are pct of entry and are converted back to r-multiples here, because the
// PositionManager measures them against the placed stop.
//
// cfg.sl_pct must already be the resolved 1R (the risk block's sl_pct, as the
// scan/wfa/replay paths set it) -- Build throws otherwise.
//
// trade_management.break_even.enabled == false arms break-even at infinity and
// its target at 0R, so Build produces the same ladder as
// ApplyRMultiplesManagement. the levels themselves stay required either way.
// --- config surface (authoritative; mirrors src/factory.cpp) ---------------
//
// AppConfig carries triggers/filters/risk modules as opaque json, so this is
// the full list of names and the params each reader consumes. Every param
// below is OPTIONAL and falls back to the shown default: a missing or
// misspelled key never fails, it silently takes the default. This is the
// opposite of the scalar environment/risk/trade_management keys, which are
// strict. If a param must be guaranteed, add it to AppConfig, not here.
//
// triggers (name: params):
//   DeltaAbsorption:    delta=1.0
//   StackedImbalance:   ratio=1.5, levels=2
//   SpoofHunter:        lookback_ms=500, min_wall_qty=30.0, drop_threshold=0.9
// filters:
//   MinVolume:          min_volume=1.0
//   VwapTrend:          require_trend_alignment=true
//   CVDDivergence:      lookback=5
//   MacroTrend:         lookback=1440
//   Volatility:         min_dollar=150.0, lookback=5
//   TimeOfDay:          start_h=8, start_m=0, end_h=17, end_m=0
//   POCTrend:           (none)
//   RVOL:               threshold=1.5, lookback=20
//   ATRChop:            min_atr=20.0, lookback=14
//   OrderbookImbalance: ratio=3.0   <- a FILTER; not a valid trigger name
// risk.modules:
//   SinglePositionLock:  (none)
//   MaxStopDistanceLock: max_stop_pct=0.015
//   MaxLeverageLock:     max_leverage=10.0
//   AntiRevengeLock:     cooldown_ms=1800000
//
// any component may be disabled with "enabled"/"active"/"isActive" set to
// false, "false"/"0", or 0. disabling every trigger is a build error.
//
// an unknown name is a build error, not a no-op: the run aborts before data is
// loaded. AppConfig::Load cannot catch it, because it never interprets names.
//
// use_paper_broker (only meaningful when live_mode == true) is the shadow /
// dry-run switch (--paper): the real market-data streamer is still built from
// the venue spec, but no execution adapter is constructed. engine.broker stays
// null and every module binds to the PaperTrader, seeded from
// cfg.initial_balance -- live data, live signals, local fills, no order ever
// leaving the process.
class StrategyFactory {
public:
  static EngineInstance Build(const AppConfig &cfg, bool live_mode = false,
                              bool use_paper_broker = false);
};
