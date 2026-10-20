#pragma once
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>

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
  std::string exchange = "binance";
  std::string api_key = "";
  std::string api_secret = "";
  // fail safe: default to testnet so a missing or misspelled key in the
  // config can never silently route live orders to api.bybit.com.
  bool is_testnet = true;
  int interval_ms = 60000;
  int macro_interval_ms = 900000;
  int vwap_reset_hour = 0;
  double tick_size = 1.0;
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
  double tp_pct = 0.01;
  double risk_per_trade_pct = 0.01;
  double min_distance_dollars = 0.0;
  double max_daily_loss = 400.0;

  // strict validation of all critical bounds.
  // throws std::invalid_argument with a precise message on the first violation.
  void Validate() const;

  // parse json to struct.
  // throws std::runtime_error with the failing section name on json type errors.
  static AppConfig Load(const json& j);
};

// recursively walk a json tree and collapse any grid definition object
// ({"min":..,"max":..,"step":..,"val":..}) into its scalar "val" field.
// used by the live path so the factory only ever sees plain numbers.
// the backtest path keeps the original grid objects for GridScanner.
void FlattenGrids(json& j);
