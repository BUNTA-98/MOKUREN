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

// trade management settings
struct TradeManagementConfig {
  double be_trigger_pct = 0.005;
  double be_target_pct = 0.001;
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

  // risk limits
  double sl_pct = 0.002;
  double tp_pct = 0.01;
  double risk_per_trade_pct = 0.01;
  double min_distance_dollars = 10.0;
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
