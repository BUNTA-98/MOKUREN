#pragma once
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <thread>

using json = nlohmann::json;

struct AppConfig {
  int interval_ms = 60000;
  int macro_interval_ms = 900000;
  double tick_size = 1.0;

  unsigned int max_cores = 0;

  std::string trigger_name;
  std::vector<std::string> filter_names;

  double risk_per_trade_pct = 0.01;
  double min_distance_dollars = 10.0;

  double d_min, d_max, d_step;
  double sl_min, sl_max, sl_step;
  double tp_min, tp_max, tp_step;

  // config aus datei laden
  static AppConfig Load(const std::string &path) {
    std::ifstream file(path);
    if (!file.is_open()) {
      std::cerr << "fehler: config.json nicht gefunden!\n";
      exit(1);
    }
    
    json j = json::parse(file);
    AppConfig cfg;

    cfg.interval_ms = j["environment"]["interval_ms"];
    cfg.macro_interval_ms = j["environment"]["macro_interval_ms"];
    cfg.tick_size = j["environment"]["tick_size"];

    // cores aus config laden mit fallback
    if (j["environment"].contains("max_cores") && j["environment"]["max_cores"] > 0) {
      cfg.max_cores = j["environment"]["max_cores"];
    } else {
      cfg.max_cores = std::thread::hardware_concurrency();
      if (cfg.max_cores == 0) cfg.max_cores = 4;
    }

    cfg.trigger_name = j["strategy"]["trigger"];
    for (const auto &f : j["strategy"]["filters"]) {
      cfg.filter_names.push_back(f);
    }

    cfg.risk_per_trade_pct = j["risk"]["risk_per_trade_pct"];
    cfg.min_distance_dollars = j["risk"]["min_distance_dollars"];

    cfg.d_min = j["grid_search"]["delta"]["min"];
    cfg.d_max = j["grid_search"]["delta"]["max"];
    cfg.d_step = j["grid_search"]["delta"]["step"];

    cfg.sl_min = j["grid_search"]["sl_pct"]["min"];
    cfg.sl_max = j["grid_search"]["sl_pct"]["max"];
    cfg.sl_step = j["grid_search"]["sl_pct"]["step"];

    cfg.tp_min = j["grid_search"]["tp_pct"]["min"];
    cfg.tp_max = j["grid_search"]["tp_pct"]["max"];
    cfg.tp_step = j["grid_search"]["tp_pct"]["step"];

    return cfg;
  }
};
