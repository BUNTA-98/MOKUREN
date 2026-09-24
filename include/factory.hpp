#pragma once
#include "aggregator.hpp"
#include "alpha_engine.hpp"
#include "config_parser.hpp"
#include "filter.hpp"
#include "papertrader.hpp"
#include "position_manager.hpp"
#include "position_sizer.hpp"
#include "risk_engine.hpp"
#include "strategy.hpp"
#include <memory>
#include <vector>

// container für eine komplette engine instanz
struct EngineInstance {
  std::unique_ptr<Aggregator> aggregator;
  std::unique_ptr<Aggregator> htf_aggregator;
  std::unique_ptr<PaperTrader> ptrader;
  std::unique_ptr<PositionManager> pos_manager;
  std::unique_ptr<PositionSizer> sizer;

  std::unique_ptr<ITrigger> trigger;
  std::vector<std::unique_ptr<IFilter>> filters;
  std::unique_ptr<PipelineStrategy> pipeline;
  std::unique_ptr<AlphaEngine> alpha;

  std::vector<std::unique_ptr<IRiskModule>> risk_modules;
  std::unique_ptr<RiskManager> risk_manager;
};

class StrategyFactory {
public:
  // bausatz dynamisch zusammensetzen
  static EngineInstance Build(const AppConfig &cfg, double delta, double sl, double tp) {
    EngineInstance inst;

    inst.aggregator = std::make_unique<Aggregator>(cfg.interval_ms, cfg.tick_size);
    inst.htf_aggregator = std::make_unique<Aggregator>(cfg.macro_interval_ms, cfg.tick_size);
    inst.ptrader = std::make_unique<PaperTrader>(sl, tp);
    inst.pos_manager = std::make_unique<PositionManager>(inst.ptrader.get());

    if (cfg.trigger_name == "DeltaAbsorption") {
      inst.trigger = std::make_unique<DeltaAbsorptionTrigger>(delta);
    }

    inst.pipeline = std::make_unique<PipelineStrategy>(inst.trigger.get());

    for (const auto &fname : cfg.filter_names) {
      if (fname == "MinVolume") {
        inst.filters.push_back(std::make_unique<MinVolumeFilter>(1.0));
        inst.pipeline->AddFilter(inst.filters.back().get());
      } else if (fname == "MacroTrend") {
        inst.filters.push_back(std::make_unique<MacroTrendFilter>());
        inst.pipeline->AddFilter(inst.filters.back().get());
      }
    }
    
    inst.alpha = std::make_unique<AlphaEngine>(*inst.pipeline);
    inst.sizer = std::make_unique<PositionSizer>(*inst.ptrader, cfg.risk_per_trade_pct, cfg.min_distance_dollars);
    inst.risk_manager = std::make_unique<RiskManager>();

    inst.risk_modules.push_back(std::make_unique<SinglePositionLock>(*inst.ptrader));
    inst.risk_modules.push_back(std::make_unique<MaxLeverageLock>(*inst.ptrader, 10.0));

    for (auto &mod : inst.risk_modules) {
      inst.risk_manager->AddModule(mod.get());
    }

    return inst;
  }
};