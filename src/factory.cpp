#include "factory.hpp"

#include "bybit_broker.hpp"
#include "bybit_streamer.hpp"
#include "hyperliquid_broker.hpp"
#include "hyperliquid_streamer.hpp"
#include "position_sizer.hpp"
#include "venue_spec.hpp"

#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

// mirrors GridScanner::FindParams: a component is disabled by a bool false,
// the strings "false"/"0", or the integer 0. the old code called
// params["active"].get<bool>(), which throws on exactly the string and
// integer spellings that GridScanner accepts.
bool IsDisabled(const json& params) {
  for (const char* key : {"enabled", "active", "isActive"}) {
    if (!params.contains(key)) continue;
    const auto& v = params[key];
    if (v.is_boolean()) {
      if (!v.get<bool>()) return true;
    } else if (v.is_string()) {
      const std::string s = v.get<std::string>();
      if (s == "false" || s == "False" || s == "FALSE" || s == "0") return true;
    } else if (v.is_number_integer()) {
      if (v.get<long long>() == 0) return true;
    } else if (v.is_number_float()) {
      if (v.get<double>() == 0.0) return true;
    }
  }
  return false;
}

// numeric param reader tolerant of the string spellings left behind by
// FlattenGrids and hand-written configs. mirrors AppConfig::Load's SafeNumber.
double ParamNumber(const json& params, const char* key, double fallback) {
  if (!params.contains(key)) return fallback;
  const auto& v = params[key];
  if (v.is_number()) return v.get<double>();
  if (v.is_string()) {
    try {
      return std::stod(v.get<std::string>());
    } catch (...) {
      return fallback;
    }
  }
  return fallback;
}

int ParamInt(const json& params, const char* key, int fallback) {
  return static_cast<int>(ParamNumber(params, key, static_cast<double>(fallback)));
}

// bool reader with the same tolerance. a plain params.value(key, true) throws
// when the value is the string or integer spelling that IsDisabled already
// accepts.
bool ParamBool(const json& params, const char* key, bool fallback) {
  if (!params.contains(key)) return fallback;
  const auto& v = params[key];
  if (v.is_boolean()) return v.get<bool>();
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    if (s == "true" || s == "True" || s == "TRUE" || s == "1") return true;
    if (s == "false" || s == "False" || s == "FALSE" || s == "0") return false;
    return fallback;
  }
  if (v.is_number()) return v.get<double>() != 0.0;
  return fallback;
}

// resolve one trigger by name. an unknown name is a config error, not a
// no-op: the previous code left inst.trigger null (or an OR_Trigger with no
// children), which evaluates to NONE for the entire run.
std::unique_ptr<ITrigger> MakeTrigger(const std::string& name, const json& p) {
  if (name == "DeltaAbsorption") {
    // the configs (and their grid nodes) say min_delta_threshold; this read
    // "delta", so a configured threshold never reached the trigger and it ran
    // at the 1.0 fallback. same class of bug as StackedImbalance's
    // ratio / ratio_threshold.
    auto num_any = [&p](std::initializer_list<const char*> keys, double fb) {
      for (const char* k : keys) {
        if (p.contains(k)) return ParamNumber(p, k, fb);
      }
      return fb;
    };
    const double min_delta = num_any({"min_delta_threshold", "delta"}, 1.0);
    // a threshold of 0 means "any net delta", which is every absorption
    // candle ever printed; the grid below would then spend its budget on a
    // trigger that fires on everything. reject it as a config error instead of
    // silently flooring it, and let grid_runner's preflight name the
    // permutation and the swept value.
    if (!std::isfinite(min_delta) || min_delta <= 0.0) {
      throw std::runtime_error(
          "factory: DeltaAbsorption.min_delta_threshold must be a finite "
          "number > 0 in base-asset units (got " +
          std::to_string(min_delta) + "); a threshold of 0 fires on every "
          "candle whose delta has the right sign");
    }
    return std::make_unique<DeltaAbsorptionTrigger>(min_delta);
  }
  if (name == "StackedImbalance") {
    // Read BOTH spellings, canonical first. The configs (and every grid node in
    // them) say ratio_threshold / min_stacked_count, while this factory used to
    // read ratio / levels -- so a grid over ratio_threshold never reached the
    // trigger: every permutation silently ran at the 1.5 fallback, and the swept
    // axis could not change a single result. The second parameter is the stack
    // LENGTH, not a book depth, so `levels` was also the wrong key: the trigger
    // armed at 2 levels instead of the configured 3.
    auto num_any = [&p](std::initializer_list<const char*> keys, double fb) {
      for (const char* k : keys) {
        if (p.contains(k)) return ParamNumber(p, k, fb);
      }
      return fb;
    };
    const double ratio = num_any({"ratio_threshold", "ratio"}, 1.5);
    const int stacked =
        static_cast<int>(num_any({"min_stacked_count", "levels"}, 2.0));
    // significance floor per level, in base-asset units. defaults to the old
    // hardcoded 0.0001 so an untouched config behaves as before apart from the
    // one-sided short-circuit the trigger itself no longer performs.
    const double min_qty = num_any({"min_level_qty", "min_qty"}, 0.0001);
    // absent means "do not gate on candle direction": that decision belongs to
    // the filter chain. a config that wants it says so explicitly.
    const bool agree = ParamBool(p, "require_candle_agreement", false);
    // no cooldown argument on purpose: re-entry spacing is a risk module
    // (AntiRevengeLock), not a property of the tape.
    return std::make_unique<StackedImbalanceTrigger>(
        ratio, stacked, min_qty, agree);
  }
  if (name == "SpoofHunter") {
    return std::make_unique<SpoofHunterTrigger>(
        ParamInt(p, "lookback_ms", 500),
        ParamNumber(p, "min_wall_qty", 30.0),
        ParamNumber(p, "drop_threshold", 0.9));
  }
  throw std::runtime_error("factory: unknown trigger '" + name +
                           "' (supported: DeltaAbsorption, StackedImbalance, "
                           "SpoofHunter)");
}

} // namespace

EngineInstance StrategyFactory::Build(const AppConfig &cfg, bool live_mode,
                                      bool use_paper_broker) {
  EngineInstance inst;

  // --paper is a streaming mode: it changes the execution target, not the data
  // source. accepting it on a backtest would silently pretend to shadow.
  if (use_paper_broker && !live_mode) {
    throw std::runtime_error(
        "factory: use_paper_broker requires live_mode (shadow mode streams "
        "real market data)");
  }

  // initialize core engine parts
  inst.aggregator = std::make_unique<Aggregator>(cfg.interval_ms, cfg.tick_size, cfg.vwap_reset_hour);
  inst.htf_aggregator = std::make_unique<Aggregator>(cfg.macro_interval_ms, cfg.tick_size, cfg.vwap_reset_hour);
  
  // the starting balance is account state, not an execution-mode detail. only
  // --paper took cfg.initial_balance; every other path kept the 10000 constant
  // baked into the constructor, so a config that said 25000 produced a scan
  // whose risk-per-trade, drawdown percentages and prop limits were all
  // measured against a different account than the one it described. one seed
  // for every path. a --live run never reads ptrader (it has a venue adapter),
  // so this only moves the paper/scan/wfa/replay numbers -- which is the point.
  const double seed_balance = cfg.initial_balance;
  inst.ptrader = std::make_unique<PaperTrader>(cfg.max_daily_loss,
                                               cfg.slippage_pct,
                                               cfg.taker_fee_pct,
                                               seed_balance);

  // execution venue AND market data, resolved from one spec, so the broker and
  // the streamer can never disagree about environment, host, category or
  // symbol. the previous split -- a testnet broker default next to a mainnet
  // streamer default -- was a real-money/real-data mismatch waiting for its
  // first config.
  if (cfg.exchange == "bybit") {
    const mokuren::venue::Spec spec =
        mokuren::venue::Resolve(cfg.env, cfg.category, cfg.symbol);

    // explicit arguments: the streamer must never fall back to its own
    // compiled-in (mainnet) default host.
    inst.streamer =
        std::make_unique<BybitDataStreamer>(spec.symbol, spec.public_ws);

    if (live_mode && !use_paper_broker) {
      inst.broker =
          std::make_unique<BybitBroker>(cfg.api_key, cfg.api_secret, spec);
      LOG_INFO("factory: bybit live {}", mokuren::venue::Manifest(spec));
      if (cfg.position_mode != "one_way") {
        LOG_WARN("factory: venue.position_mode='{}' is not implemented; the "
                 "adapter sends positionIdx=0 (one-way) orders",
                 cfg.position_mode);
      }
    } else if (live_mode) {
      // shadow mode: the public streamer above is real, but no execution
      // adapter is built. engine.broker stays null, so ActiveBroker() and every
      // IBroker consumer below bind to the PaperTrader.
      LOG_INFO("factory: bybit shadow (paper execution) {}; no orders will be "
               "sent",
               mokuren::venue::Manifest(spec));
    } else {
      LOG_INFO("factory: bybit data-only {}; using the PaperTrader",
               mokuren::venue::Manifest(spec));
    }
  } else if (cfg.exchange == "hyperliquid") {
    if (live_mode && !use_paper_broker) {
      // no hyperliquid execution adapter is wired here, so a --live run has
      // nothing to send orders to. --paper is the supported rehearsal: real
      // public market data, local fills.
      throw std::runtime_error(
          "factory: no execution adapter for exchange 'hyperliquid' "
          "(--paper streams hyperliquid data into the PaperTrader)");
    }
    if (live_mode) {
      // shadow mode: the public streamer is real, but no execution adapter is
      // built. inst.broker stays null, so ActiveBroker() and every IBroker
      // consumer below bind to the PaperTrader.
      //
      // the coin is cfg.symbol verbatim. hyperliquid subscribes by bare coin
      // ("BTC"), so translating a pair name behind the operator's back would
      // make a config mean something it does not say; say it out loud instead.
      if (cfg.symbol.find("USDT") != std::string::npos ||
          cfg.symbol.find("USDC") != std::string::npos) {
        LOG_WARN("factory: venue.symbol='{}' looks like a pair, but hyperliquid "
                 "subscribes by bare coin (e.g. 'BTC'); the venue will reject "
                 "this subscription",
                 cfg.symbol);
      }
      // environment selects the endpoint, exactly as it does for bybit through
      // venue::Resolve. the streamer's default url is mainnet, so a config that
      // said "testnet" used to stream mainnet data with nothing to show for it
      // -- the same broker/streamer disagreement the shared venue spec was
      // introduced to remove.
      if (cfg.env == mokuren::venue::Env::Demo) {
        LOG_WARN("factory: hyperliquid has no demo environment; using its "
                 "testnet endpoint");
      }
      const char* hl_ws = cfg.env == mokuren::venue::Env::Mainnet
                              ? "wss://api.hyperliquid.xyz/ws"
                              : "wss://api.hyperliquid-testnet.xyz/ws";
      inst.streamer =
          std::make_unique<HyperliquidDataStreamer>(cfg.symbol, hl_ws);
      LOG_INFO("factory: hyperliquid shadow (paper execution) symbol={} ws={}; "
               "no orders will be sent",
               cfg.symbol, hl_ws);
    } else {
      // a backtest must never dial out to an exchange. the adapter also trades
      // nothing until its websocket state feed is populated, so constructing it
      // here turned a hyperliquid-named backtest into a silent no-op.
      LOG_INFO("factory: exchange is 'hyperliquid' but this run is not live; "
               "using the PaperTrader");
    }
  } else {
    // Validate() already rejects this, but the factory must not rely on it:
    // falling through would silently paper-trade a typo.
    throw std::runtime_error("factory: unsupported exchange '" + cfg.exchange +
                             "'");
  }
  
  // every module that talks to a broker binds to the ACTIVE one: the venue
  // adapter when the config selected a live exchange, otherwise the in-process
  // paper account.
  // NOTE: on the venue path the risk firewall and the sizer query the
  // exchange's cached state, which stays empty until the first websocket frame
  // arrives. GetBalance() is 0.0 before that, so PositionSizer yields no size
  // and MaxLeverageLock rejects every signal -- nothing trades until the state
  // feed is live.
  // in shadow mode (--paper) inst.broker is null on purpose, so this resolves
  // to the PaperTrader while the streamer keeps delivering real venue data.
  IBroker* active_broker = inst.broker ? inst.broker.get() : inst.ptrader.get();

  // wire trade management toggles (enable_scale_out & fraction)
  inst.pos_manager = std::make_unique<PositionManager>(
      active_broker,
      cfg.tm_config.enable_scale_out,
      cfg.tm_config.scale_out_fraction
  );

  // 1. build entry triggers. every named trigger must resolve: a silent null
  // trigger (or an empty OR_Trigger) evaluates to NONE for the whole run and
  // is indistinguishable from a strategy that found no setups.
  for (const auto& t : cfg.triggers) {
    if (IsDisabled(t.params)) continue;
    inst.sub_triggers.push_back(MakeTrigger(t.name, t.params));
  }

  if (inst.sub_triggers.empty()) {
    throw std::runtime_error(
        "factory: no active triggers -- every entry is disabled or empty");
  }

  if (inst.sub_triggers.size() == 1) {
    inst.trigger = std::move(inst.sub_triggers.front());
    inst.sub_triggers.clear();
  } else {
    auto or_trigger = std::make_unique<OR_Trigger>();
    for (auto& t : inst.sub_triggers) {
      or_trigger->AddTrigger(t.get());
    }
    inst.trigger = std::move(or_trigger);
  }

  inst.pipeline = std::make_unique<PipelineStrategy>(inst.trigger.get());

  // 2. build filters
  for (const auto &f : cfg.filters) {
    if (IsDisabled(f.params)) continue;

    if (f.name == "MinVolume") {
      double min_vol = ParamNumber(f.params, "min_volume", 1.0);
      inst.filters.push_back(std::make_unique<MinVolumeFilter>(min_vol));
      inst.pipeline->AddFilter(inst.filters.back().get());
    
    } else if (f.name == "VwapTrend") {
      bool require_trend = ParamBool(f.params, "require_trend_alignment", true);
      inst.filters.push_back(std::make_unique<VwapTrendFilter>(require_trend));
      inst.pipeline->AddFilter(inst.filters.back().get());
    
    } else if (f.name == "CVDDivergence") {
      int lookback = ParamInt(f.params, "lookback", 5);
      inst.filters.push_back(std::make_unique<CVDDivergenceFilter>(lookback));
      inst.pipeline->AddFilter(inst.filters.back().get());

    } else if (f.name == "MacroTrend") {
      int lookback = ParamInt(f.params, "lookback", 1440);
      inst.filters.push_back(std::make_unique<MacroTrendFilter>(lookback));
      inst.pipeline->AddFilter(inst.filters.back().get());
    
    } else if (f.name == "Volatility") {
      double min_dl = ParamNumber(f.params, "min_dollar", 150.0);
      int lookback = ParamInt(f.params, "lookback", 5);
      inst.filters.push_back(std::make_unique<VolatilityFilter>(min_dl, lookback));
      inst.pipeline->AddFilter(inst.filters.back().get());
    
    } else if (f.name == "TimeOfDay") {
      int start_h = ParamInt(f.params, "start_h", 8);
      int start_m = ParamInt(f.params, "start_m", 0);
      int end_h = ParamInt(f.params, "end_h", 17);
      int end_m = ParamInt(f.params, "end_m", 0);
      inst.filters.push_back(std::make_unique<TimeOfDayFilter>(start_h, start_m, end_h, end_m));
      inst.pipeline->AddFilter(inst.filters.back().get());

    } else if (f.name == "POCTrend") {
      inst.filters.push_back(std::make_unique<POCTrendFilter>());
      inst.pipeline->AddFilter(inst.filters.back().get());
    
    } else if (f.name == "RVOL") { 
      double threshold = ParamNumber(f.params, "threshold", 1.5);
      int lookback = ParamInt(f.params, "lookback", 20);
      inst.filters.push_back(std::make_unique<RVOLFilter>(threshold, lookback));
      inst.pipeline->AddFilter(inst.filters.back().get());
  
    } else if (f.name == "ATRChop") { 
      double min_atr = ParamNumber(f.params, "min_atr", 20.0);
      int period = ParamInt(f.params, "lookback", 14);
      inst.filters.push_back(std::make_unique<ATRFilter>(min_atr, period));
      inst.pipeline->AddFilter(inst.filters.back().get());

    } else if (f.name == "OrderbookImbalance") {
      double ratio = ParamNumber(f.params, "ratio", 3.0);
      inst.filters.push_back(std::make_unique<OrderbookImbalanceFilter>(ratio));
      inst.pipeline->AddFilter(inst.filters.back().get());

    } else {
      throw std::runtime_error("factory: unknown filter '" + f.name + "'");
    }
  }
  
  // 3. wire evaluation and risk modules
  inst.alpha = std::make_unique<AlphaEngine>(*inst.pipeline);
  // leverage policy has ONE owner: MaxLeverageLock, wired in the module loop
  // below. The sizer used to carry a second, independently configured cap,
  // which meant a structural stop implying 40x was silently halved to 10x and
  // then recorded as if it had risked the full risk_per_trade_pct -- making
  // rows with different stop widths incomparable. leverage == risk_pct /
  // stop_pct, so a size cap and a leverage veto are the same decision; keeping
  // both, at different numbers, was the duplication. The sizer now holds only
  // the physical ceiling, which the factory guarantees the policy never
  // exceeds, so the lock always decides first.
  //
  // lot_step is the venue's minimum quantity increment (BTCUSDT linear: 0.001
  // BTC, ETH: 0.01, and so on). a hardcoded 0.001 is right for BTC and wrong
  // everywhere else, so it travels with venue.symbol.
  inst.sizer = std::make_unique<PositionSizer>(active_broker, cfg.risk_per_trade_pct,
                                               cfg.min_distance_dollars, cfg.lot_step,
                                               kVenueMaxLeverage);
  inst.risk_manager = std::make_unique<RiskManager>();

  for (const auto &r : cfg.risk_modules) {
    if (IsDisabled(r.params)) continue;

    if (r.name == "SinglePositionLock") {
      inst.risk_modules.push_back(std::make_unique<SinglePositionLock>(active_broker));
    } else if (r.name == "MaxStopDistanceLock") {
      double max_pct = ParamNumber(r.params, "max_stop_pct", 0.015);
      inst.risk_modules.push_back(std::make_unique<MaxStopDistanceLock>(max_pct));
    } else if (r.name == "MaxLeverageLock") {
      // the only leverage policy in the engine. validate the number instead of
      // trusting it: <= 0 rejects every signal (leverage is always > 0), and
      // above the physical ceiling the sizer would clamp first and this lock
      // could never fire -- the silent-resize bug this replaced.
      const double max_lev = ParamNumber(r.params, "max_leverage", 10.0);
      if (!std::isfinite(max_lev) || max_lev <= 0.0 ||
          max_lev > kVenueMaxLeverage) {
        throw std::runtime_error(
            "factory: MaxLeverageLock.max_leverage must be in (0.0, " +
            std::to_string(kVenueMaxLeverage) + "] (got " +
            std::to_string(max_lev) + ")");
      }
      inst.risk_modules.push_back(std::make_unique<MaxLeverageLock>(active_broker, max_lev));
    } else if (r.name == "AntiRevengeLock") {
      int64_t cooldown =
          static_cast<int64_t>(ParamNumber(r.params, "cooldown_ms", 1800000.0));
      inst.risk_modules.push_back(std::make_unique<AntiRevengeLock>(active_broker, cooldown));

    } else {
      // dropping a risk module by typo removes a safety lock; fail loudly.
      throw std::runtime_error("factory: unknown risk module '" + r.name + "'");
    }
  }

  for (auto &mod : inst.risk_modules) {
    inst.risk_manager->AddModule(mod.get());
  }

  // exactly one management owner: PositionManager runs break-even, trailing
  // and scale-out on every path. PaperTrader keeps only the stop and the
  // target, so arming its ladder as well would move the stop and scale out
  // twice per trade.
  inst.ptrader->DisableManagement();

  // arm the ladder from cfg.tm_config. this used to be handed only to
  // PaperTrader::ApplyManagementConfig, so a live run -- which calls
  // Configure nowhere else -- silently used PositionManager's 1R/1R defaults.
  //
  // tm_config stores each level as a pct of entry (r * sl_pct, resolved once
  // at the config boundary), while PositionManager measures every threshold
  // against the stop actually placed. divide by sl_pct to recover the r the
  // author wrote: passing the pct straight through armed break-even at
  // 0.005R instead of 1R. the backtest path masked this because
  // ApplyRMultiplesManagement re-configures afterwards; live has no second
  // call, which is exactly where the wrong ladder would have shipped.
  if (!(cfg.sl_pct > 0.0)) {
    throw std::runtime_error(
        "factory: sl_pct must be > 0; it defines 1R for trade management");
  }
  const double inv_sl = 1.0 / cfg.sl_pct;

  // an explicit break-even disable is honoured by pushing the trigger out of
  // reach, exactly as ApplyRMultiplesManagement does. the scan/wfa/replay
  // paths re-arm the ladder afterwards and masked a missing check here; live
  // has no second arming site, so a config that turned break-even off still
  // moved the stop on the venue path.
  const bool be_enabled = cfg.tm_config.enable_break_even;
  const double be_trigger_r = be_enabled
                                  ? cfg.tm_config.be_trigger_pct * inv_sl
                                  : std::numeric_limits<double>::infinity();
  const double be_target_r =
      be_enabled ? cfg.tm_config.be_target_pct * inv_sl : 0.0;

  inst.pos_manager->Configure(
      be_trigger_r, be_target_r,
      cfg.tm_config.enable_trailing,
      cfg.tm_config.trailing_trigger_pct * inv_sl,
      cfg.tm_config.trailing_dist_pct * inv_sl,
      cfg.tm_config.enable_scale_out,
      cfg.tm_config.scale_out_trigger_pct * inv_sl,
      cfg.tm_config.scale_out_fraction);

  return inst;
}
