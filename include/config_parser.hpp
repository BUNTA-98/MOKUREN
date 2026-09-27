#pragma once
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <thread>

using json = nlohmann::json;

// Speichert eine berechnete Parameter-Kombination
struct RunConfig {
  json full_json;
  std::map<std::string, double> grid_values;
};

// Hilfsstruktur für gefundene min/max/step Blöcke
struct GridParam {
  json::json_pointer ptr;
  std::string name;
  double min;
  double max;
  double step;
};

// N-Dimensionaler Parameter-Generator
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
    double step = (p.step == 0.0) ? 1.0 : p.step; // Endlosschleifen-Schutz
    for (double val = p.min; val <= p.max + 1e-9; val += step) {
      json next_json = base_json;
      next_json[p.ptr] = val; // Überschreibt {min,max,step} mit dem konkreten Wert
      
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
      results.push_back({root, initial_vals}); // Kein Grid-Search, nur 1 Run
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

struct AppConfig {
  int interval_ms = 60000;
  int macro_interval_ms = 900000;
  double tick_size = 1.0;
  unsigned int max_cores = 0;

  std::vector<ComponentConfig> triggers;
  std::vector<ComponentConfig> filters;
  std::vector<ComponentConfig> risk_modules;

  double sl_pct = 0.002;
  double tp_pct = 0.01;
  double risk_per_trade_pct = 0.01;
  double min_distance_dollars = 10.0;
  double max_daily_loss = 400.0;
  double slippage_pct = 0.0002;

  // Lade direkt aus dem bereiten JSON
  static AppConfig Load(const json& j) {
    AppConfig cfg;

    cfg.interval_ms = j["environment"].value("interval_ms", 60000);
    cfg.macro_interval_ms = j["environment"].value("macro_interval_ms", 900000);
    cfg.tick_size = j["environment"].value("tick_size", 1.0);
    cfg.slippage_pct = j["environment"].value("slippage_pct", 0.0002);

    if (j["environment"].contains("max_cores") && j["environment"]["max_cores"] > 0) {
      cfg.max_cores = j["environment"]["max_cores"];
    } else {
      cfg.max_cores = std::thread::hardware_concurrency();
      if (cfg.max_cores == 0) cfg.max_cores = 4;
    }

    if (j["strategy"].contains("triggers")) {
      for (const auto &item : j["strategy"]["triggers"]) {
        cfg.triggers.push_back({item["name"], item});
      }
    }

    if (j["strategy"].contains("filters")) {
      for (const auto &item : j["strategy"]["filters"]) {
        cfg.filters.push_back({item["name"], item});
      }
    }

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

    return cfg;
  }
};
