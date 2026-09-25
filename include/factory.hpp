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

struct EngineInstance {
  std::unique_ptr<Aggregator> aggregator;
  std::unique_ptr<Aggregator> htf_aggregator;
  std::unique_ptr<PaperTrader> ptrader;
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

class StrategyFactory {
public:
  static EngineInstance Build(const AppConfig &cfg) {
    EngineInstance inst;

    inst.aggregator = std::make_unique<Aggregator>(cfg.interval_ms, cfg.tick_size);
    inst.htf_aggregator = std::make_unique<Aggregator>(cfg.macro_interval_ms, cfg.tick_size);
    inst.ptrader = std::make_unique<PaperTrader>(cfg.sl_pct, cfg.tp_pct, cfg.max_daily_loss);
    inst.pos_manager = std::make_unique<PositionManager>(inst.ptrader.get());

    if (cfg.triggers.size() == 1) {
      if (cfg.triggers[0].name == "DeltaAbsorption") {
        double delta = cfg.triggers[0].params.value("delta", 1.0);
        inst.trigger = std::make_unique<DeltaAbsorptionTrigger>(delta);
      } else if (cfg.triggers[0].name == "StackedImbalance") {
        double ratio = cfg.triggers[0].params.value("ratio", 1.5);
        int levels = cfg.triggers[0].params.value("levels", 2);
        inst.trigger = std::make_unique<StackedImbalanceTrigger>(ratio, levels);
      }
    } else if (cfg.triggers.size() > 1) {
      auto or_trigger = std::make_unique<OR_Trigger>();
      for (const auto& t : cfg.triggers) {
        if (t.name == "DeltaAbsorption") {
          double delta = t.params.value("delta", 1.0);
          inst.sub_triggers.push_back(std::make_unique<DeltaAbsorptionTrigger>(delta));
          or_trigger->AddTrigger(inst.sub_triggers.back().get());
        } else if (t.name == "StackedImbalance") {
          double ratio = t.params.value("ratio", 1.5);
          int levels = t.params.value("levels", 2);
          inst.sub_triggers.push_back(std::make_unique<StackedImbalanceTrigger>(ratio, levels));
          or_trigger->AddTrigger(inst.sub_triggers.back().get());
        }
      }
      inst.trigger = std::move(or_trigger);
    }

    inst.pipeline = std::make_unique<PipelineStrategy>(inst.trigger.get());

    for (const auto &f : cfg.filters) {
      if (f.name == "MinVolume") {
        double min_vol = f.params.value("min_volume", 1.0);
        inst.filters.push_back(std::make_unique<MinVolumeFilter>(min_vol));
        inst.pipeline->AddFilter(inst.filters.back().get());
      } else if (f.name == "MacroTrend") {
        int lookback = f.params.value("lookback", 1440);
        inst.filters.push_back(std::make_unique<MacroTrendFilter>(lookback));
        inst.pipeline->AddFilter(inst.filters.back().get());
      } else if (f.name == "Volatility") {
        double min_dl = f.params.value("min_dollar", 150.0);
        int lookback = f.params.value("lookback", 5);
        inst.filters.push_back(std::make_unique<VolatilityFilter>(min_dl, lookback));
        inst.pipeline->AddFilter(inst.filters.back().get());
      }
    }
    
    inst.alpha = std::make_unique<AlphaEngine>(*inst.pipeline);
    inst.sizer = std::make_unique<PositionSizer>(*inst.ptrader, cfg.risk_per_trade_pct, cfg.min_distance_dollars);
    inst.risk_manager = std::make_unique<RiskManager>();

    for (const auto &r : cfg.risk_modules) {
      if (r.name == "SinglePositionLock") {
        inst.risk_modules.push_back(std::make_unique<SinglePositionLock>(*inst.ptrader));
      } else if (r.name == "MaxLeverageLock") {
        double max_lev = r.params.value("max_leverage", 10.0);
        inst.risk_modules.push_back(std::make_unique<MaxLeverageLock>(*inst.ptrader, max_lev));
      } else if (r.name == "AntiRevengeLock") {
        int64_t cooldown = r.params.value("cooldown_ms", 1800000);
        inst.risk_modules.push_back(std::make_unique<AntiRevengeLock>(*inst.ptrader, cooldown));
      }
    }

    for (auto &mod : inst.risk_modules) {
      inst.risk_manager->AddModule(mod.get());
    }

    return inst;
  }
};