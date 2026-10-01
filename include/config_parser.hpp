#pragma once
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <thread>

using json = nlohmann::json;

struct RunConfig {
  json full_json;
  std::map<std::string, double> grid_values;
};

struct GridParam {
  json::json_pointer ptr;
  std::string name;
  double min;
  double max;
  double step;
};

class GridScanner {
private:
  static void FindParams(const json& j, const std::string& current_path, std::vector<GridParam>& params) {
    if (j.is_object()) {
      if (j.contains("min") && j.contains("max") && j.contains("step")) {
        std::string param_name = current_path.substr(current_path.find_last_of('/') + 1);
        params.push_back({json::json_pointer(current_path), param_name, 
                          j["min"].get<double>(), j["max"].get<double>(), j["step"].get<double>()});
      } else {
        for (auto it = j.begin(); it != j.end(); ++it) {
          FindParams(it.value(), current_path + "/" + it.key(), params);
        }
      }
    } else if (j.is_array()) {
      for (size_t i = 0; i < j.size(); ++i) {
        FindParams(j[i], current_path + "/" + std::to_string(i), params);
      }
    }
  }

  static void Generate(const json& base_json, const std::vector<GridParam>& params, size_t index, 
                       std::map<std::string, double>& current_vals, std::vector<RunConfig>& results) {
    if (index >= params.size()) {
      results.push_back({base_json, current_vals});
      return;
    }

    const auto& p = params[index];
    double step = (p.step == 0.0) ? 1.0 : p.step; 
    for (double val = p.min; val <= p.max + 1e-9; val += step) {
      json next_json = base_json;
      next_json[p.ptr] = val; 
      
      std::map<std::string, double> next_vals = current_vals;
      next_vals[p.name] = val;
      
      Generate(next_json, params, index + 1, next_vals, results);
    }
  }

public:
  static std::vector<RunConfig> GenerateGrid(const json& root) {
    std::vector<GridParam> params;
    FindParams(root, "", params);
    
    std::vector<RunConfig> results;
    std::map<std::string, double> initial_vals;
    
    if (params.empty()) {
      results.push_back({root, initial_vals}); 
      return results;
    }
    
    Generate(root, params, 0, initial_vals, results);
    return results;
  }
};

struct ComponentConfig {
  std::string name;
  json params;
};

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

struct AppConfig {
  // --- ENVIRONMENT (Inklusive Filepath!) ---
  std::string filepath = "";
  int interval_ms = 60000;
  int macro_interval_ms = 900000;
  int vwap_reset_hour = 0;
  double tick_size = 1.0;
  unsigned int max_cores = 0;
  double slippage_pct = 0.0002;

  // --- MODULES ---
  std::vector<ComponentConfig> triggers;
  std::vector<ComponentConfig> filters;
  std::vector<ComponentConfig> risk_modules;
  
  // --- TRADE MANAGEMENT ---
  TradeManagementConfig tm_config;

  // --- RISK ---
  double sl_pct = 0.002;
  double tp_pct = 0.01;
  double risk_per_trade_pct = 0.01;
  double min_distance_dollars = 10.0;
  double max_daily_loss = 400.0;

  static AppConfig Load(const json& j) {
    AppConfig cfg;

    if (j.contains("environment")) {
      cfg.filepath = j["environment"].value("filepath", "");
      cfg.interval_ms = j["environment"].value("interval_ms", 60000);
      cfg.macro_interval_ms = j["environment"].value("macro_interval_ms", 900000);
      cfg.tick_size = j["environment"].value("tick_size", 1.0);
      cfg.slippage_pct = j["environment"].value("slippage_pct", 0.0002);
      cfg.vwap_reset_hour = j["environment"].value("vwap_reset_hour", 0);

      if (j["environment"].contains("max_cores") && j["environment"]["max_cores"] > 0) {
        cfg.max_cores = j["environment"]["max_cores"];
      } else {
        cfg.max_cores = std::thread::hardware_concurrency();
        if (cfg.max_cores == 0) cfg.max_cores = 4;
      }
    }

    if (j.contains("triggers")) {
      for (const auto &item : j["triggers"]) cfg.triggers.push_back({item["name"], item});
    } else if (j.contains("strategy") && j["strategy"].contains("triggers")) {
      for (const auto &item : j["strategy"]["triggers"]) cfg.triggers.push_back({item["name"], item});
    }

    if (j.contains("filters")) {
      for (const auto &item : j["filters"]) cfg.filters.push_back({item["name"], item});
    } else if (j.contains("strategy") && j["strategy"].contains("filters")) {
      for (const auto &item : j["strategy"]["filters"]) cfg.filters.push_back({item["name"], item});
    }

    if (j.contains("risk")) {
      cfg.sl_pct = j["risk"].value("sl_pct", 0.002);
      cfg.tp_pct = j["risk"].value("tp_pct", 0.01);
      cfg.risk_per_trade_pct = j["risk"].value("risk_per_trade_pct", 0.01);
      cfg.min_distance_dollars = j["risk"].value("min_distance_dollars", 10.0);
      cfg.max_daily_loss = j["risk"].value("max_daily_loss", 400.0);

      if (j["risk"].contains("modules")) {
        for (const auto &item : j["risk"]["modules"]) {
          cfg.risk_modules.push_back({item["name"], item});
        }
      }
    }

    if (j.contains("trade_management")) {
      const auto& tm = j["trade_management"];
      if (tm.contains("break_even")) {
        cfg.tm_config.be_trigger_pct = tm["break_even"].value("trigger_pct", 0.005);
        cfg.tm_config.be_target_pct = tm["break_even"].value("target_pct", 0.001);
      }
      if (tm.contains("trailing")) {
        cfg.tm_config.enable_trailing = tm["trailing"].value("enabled", true);
        cfg.tm_config.trailing_trigger_pct = tm["trailing"].value("trigger_pct", 0.008);
        cfg.tm_config.trailing_dist_pct = tm["trailing"].value("distance_pct", 0.004);
      }
      if (tm.contains("scale_out")) {
        cfg.tm_config.enable_scale_out = tm["scale_out"].value("enabled", false);
        cfg.tm_config.scale_out_trigger_pct = tm["scale_out"].value("trigger_pct", 0.006);
        cfg.tm_config.scale_out_fraction = tm["scale_out"].value("fraction", 0.5);
      }
    }

    return cfg;
  }
};
