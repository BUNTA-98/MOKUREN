#pragma once
#include "aggregator.hpp"
#include "alpha_engine.hpp"
#include "config_parser.hpp"
#include "filter.hpp"
#include "ibroker.hpp"
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
  // live execution venue. null on the paper/backtest path; set to a
  // HyperliquidBroker when the config selects exchange "hyperliquid".
  // when set, LiveOrchestrator::ActiveBroker() and every module that takes an
  // IBroker* (PositionManager, PositionSizer, the risk firewall) are bound to
  // it instead of ptrader -- ptrader remains constructed but is bypassed.
  std::unique_ptr<IBroker> broker;
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
class StrategyFactory {
public:
  static EngineInstance Build(const AppConfig &cfg, bool live_mode = false);
};
