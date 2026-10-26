#pragma once
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>

#include "prop_guard.hpp"
#include "startup_gate.hpp"
#include "venue_spec.hpp"

using json = nlohmann::json;

// holds json and specific grid values for one run
struct RunConfig {
  json full_json;
  std::map<std::string, double> grid_values;
};

// structure for grid parameters
struct GridParam {
  json::json_pointer ptr;
  std::string name;
  double min;
  double max;
  double step;
};

class GridScanner {
private:
  // recursive search for grid definitions (min, max, step)
  static void FindParams(const json& j, const std::string& current_path, std::vector<GridParam>& params);

  // generate combinations recursively
  static void Generate(const json& base_json, const std::vector<GridParam>& params, size_t index,
                       std::map<std::string, double>& current_vals, std::vector<RunConfig>& results);

public:
  // main entry point for grid generation
  static std::vector<RunConfig> GenerateGrid(const json& root);
};

// generic component config structure
struct ComponentConfig {
  std::string name;
  json params;
};

// trade management settings. AppConfig::Load fills these through
// mokuren::ResolveLevelPct: the *_r key is canonical and sl_pct defines 1R,
// the legacy absolute *_pct key is honoured only when the R key is absent.
// A missing level is a hard error; Load never substitutes a default.
struct TradeManagementConfig {
  double be_trigger_pct = 0.005;
  double be_target_pct = 0.001;
  // optional in the config: break_even historically carried no "enabled" key,
  // and an absent flag means enabled. carried on the struct so the live path
  // -- where StrategyFactory::Build is the only arming site -- honours an
  // explicit "enabled": false the same way ApplyRMultiplesManagement does in
  // the backtest paths.
  bool enable_break_even = true;
  bool enable_trailing = true;
  double trailing_trigger_pct = 0.008;
  double trailing_dist_pct = 0.004;
  bool enable_scale_out = false;
  double scale_out_trigger_pct = 0.006;
  double scale_out_fraction = 0.5;
};

// main application config
struct AppConfig {
  // environment settings
  std::string filepath = "";
  // never default the venue. this used to say "binance", which meant a
  // directly-constructed AppConfig (tests, tooling) streamed a venue nobody
  // selected. Load() already refuses a config without the key; the default is
  // only a safety net for code that bypasses Load, so it is empty and fails
  // Validate() loudly.
  std::string exchange = "";
  std::string api_key = "";
  std::string api_secret = "";
  // fail safe: default to testnet so a missing or misspelled key in the
  // config can never silently route live orders to api.bybit.com.
  //
  // LEGACY: a bool cannot express demo, so this only distinguishes testnet
  // from mainnet. `env` below is authoritative; this field is kept so old
  // configs keep working and so nothing reads two different truths.
  bool is_testnet = true;

  // venue identity. one object the broker and the market-data streamer both
  // take, so they cannot disagree about host, category or symbol.
  mokuren::venue::Env env = mokuren::venue::Env::Testnet;
  std::string category = "linear";
  std::string symbol = "BTCUSDT";
  std::string position_mode = "one_way"; // "one_way" | "hedge"
  // mainnet spends real money. requiring an explicit opt-in turns a config
  // typo into a startup abort instead of a funded order.
  bool allow_mainnet = false;

  // funded-account survival limits (hyrotrader: 4% daily, 6% total). enforced
  // by one guard on every path, independent of sl_pct/tp_r -- a strategy's
  // per-trade risk and the account's lifetime limits are different questions.
  mokuren::PropRules prop;

  // startup data-integrity gate. the live path does not arm the engine until
  // every channel the venue profile marks required has delivered well-formed
  // frames for at least probe.min_duration_ms. Development() is the default so
  // a config that says nothing still gets checked, just quickly; set
  // "startup_probe": {"preset": "production"} for the one-minute soak.
  //
  // probe.min_l2_depth is the one knob to check when switching strategies: it
  // must match the deepest "levels" param among the configured triggers, or
  // the depth triggers will index levels the feed never carried.
  mokuren::integrity::ProbeCriteria probe =
      mokuren::integrity::ProbeCriteria::Development();
  // false means "start anyway when the checks fail". only ever right for a
  // shadow run against a venue being debugged: the point of the gate is that a
  // feed which cannot produce a clean book must not be traded on.
  bool probe_abort_on_failure = true;

  int interval_ms = 60000;
  int macro_interval_ms = 900000;
  int vwap_reset_hour = 0;
  double tick_size = 1.0;
  // minimum quantity increment the venue accepts, per symbol. 0.001 is
  // BTCUSDT linear; a different instrument needs its own value, because the
  // venue rejects (or truncates) an order that is not a multiple of it.
  double lot_step = 0.001;
  unsigned int max_cores = 0;
  
  // execution and fee settings (new)
  double taker_fee_pct = 0.0004;
  double slippage_pct = 0.0002;

  // modules
  std::vector<ComponentConfig> triggers;
  std::vector<ComponentConfig> filters;
  std::vector<ComponentConfig> risk_modules;
  
  // trade management
  TradeManagementConfig tm_config;

  // risk limits.
  //
  // min_distance_dollars is an absolute quote-currency distance, not a
  // percentage: a hardcoded 10 USD floor is noise for BTC and a 6%+ stop for
  // SOL, so the default is 0 (never widen the trigger's own stop). a config
  // opts in per symbol. this must stay in sync with AppConfig::Load, which
  // reads the same key with the same 0.0 fallback -- otherwise a directly
  // constructed AppConfig (tests, tooling) behaves differently from a loaded
  // one.
  double sl_pct = 0.002;
  // take-profit distance as a fraction of price, resolved in BOTH directions:
  // AppConfig::Load runs it through mokuren::ResolveTpPct, so a config's tp_r
  // (a multiple of sl_pct, i.e. of 1R) wins over this default. a config that
  // names neither tp_r nor tp_pct fails to load instead of inheriting 1%.
  //
  // tp_pct is DERIVED when the config uses tp_r: tp_pct == tp_r * sl_pct. a
  // grid over risk.sl_pct therefore moves it without naming it. tp_r keeps the
  // multiple the config actually wrote, so Validate and the log can talk about
  // the unit the config was written in instead of a fraction nobody typed.
  double tp_pct = 0.01;
  // the R multiple tp_pct was resolved from, or 0.0 when the config named an
  // absolute tp_pct, or named neither (Load throws in that case).
  double tp_r = 0.0;
  double risk_per_trade_pct = 0.01;
  double min_distance_dollars = 0.0;
  // NOTE: there is no max_leverage here on purpose. leverage is fully
  // determined by risk_per_trade_pct / stop_pct, so a sizer-side cap and a
  // MaxLeverageLock veto are the same decision -- and only the veto is visible.
  // one number, one owner: risk.modules[MaxLeverageLock].max_leverage.
  double max_daily_loss = 400.0;

  // starting capital of the paper account, on every path. the factory hands
  // this to PaperTrader for --paper, scan, wfa and replay alike, so risk per
  // trade, drawdown percentages and the prop limits are all measured against
  // the account the config actually describes. defaults to the old
  // PaperTrader::kInitialBalance (10000) so a config that omits the key
  // reproduces the numbers it produced before.
  double initial_balance = 10000.0;

  // strict validation of all critical bounds.
  // throws std::invalid_argument with a precise message on the first violation.
  //
  // require_venue_credentials is false for a shadow run (--paper): it only
  // subscribes to the public websocket and never signs a request, so a blank
  // api key is not an error there. every path that can place an order keeps
  // the default (true) and the strict check.
  void Validate(bool require_venue_credentials = true) const;

  // parse json to struct.
  // throws std::runtime_error with the failing section name on json type errors.
  static AppConfig Load(const json& j, bool require_venue_credentials = true);
};

// recursively walk a json tree and collapse any grid definition object
// ({"min":..,"max":..,"step":..,"val":..}) into its scalar "val" field.
// used by the live path so the factory only ever sees plain numbers.
// the backtest path keeps the original grid objects for GridScanner.
void FlattenGrids(json& j);
