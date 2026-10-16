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

// parses json config and wires all components together
class StrategyFactory {
public:
  static EngineInstance Build(const AppConfig &cfg);
};
