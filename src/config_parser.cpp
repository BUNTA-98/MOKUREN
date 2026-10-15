#include "config_parser.hpp"

#include <sstream>

// ============================================================================
// GridScanner
// ============================================================================

void GridScanner::FindParams(const json& j, const std::string& current_path,
                             std::vector<GridParam>& params) {
  if (j.is_object()) {
    // skip completely if component is explicitly disabled (checks bool, string, int)
    for (const auto& key : {"enabled", "active", "isActive"}) {
      if (j.contains(key)) {
        const auto& val = j[key];
        if ((val.is_boolean() && val.get<bool>() == false) ||
            (val.is_string() && (val.get<std::string>() == "false" || val.get<std::string>() == "0")) ||
            (val.is_number_integer() && val.get<int>() == 0)) {
          return; // abort this entire branch immediately
        }
      }
    }

    // check if it is a grid node. all three keys must be present AND numeric,
    // otherwise we treat this as a regular nested object and recurse into it.
    // this prevents type_error.302 when a key like "min" holds an object.
    if (j.contains("min") && j.contains("max") && j.contains("step") &&
        j["min"].is_number() && j["max"].is_number() && j["step"].is_number()) {
      std::string param_name = current_path.substr(current_path.find_last_of('/') + 1);
      params.push_back({json::json_pointer(current_path), param_name,
                        j["min"].get<double>(), j["max"].get<double>(), j["step"].get<double>()});
    } else {
      // dig deeper into nested objects
      for (auto it = j.begin(); it != j.end(); ++it) {
        FindParams(it.value(), current_path + "/" + it.key(), params);
      }
    }
  } else if (j.is_array()) {
    // iterate through arrays
    for (size_t i = 0; i < j.size(); ++i) {
      FindParams(j[i], current_path + "/" + std::to_string(i), params);
    }
  }
}

void GridScanner::Generate(const json& base_json, const std::vector<GridParam>& params,
                           size_t index, std::map<std::string, double>& current_vals,
                           std::vector<RunConfig>& results) {
  // base case: all params processed
  if (index >= params.size()) {
    results.push_back({base_json, current_vals});
    return;
  }

  const auto& p = params[index];
  double step = (p.step == 0.0) ? 1.0 : p.step;

  // create permutations for current param
  for (double val = p.min; val <= p.max + 1e-9; val += step) {
    json next_json = base_json;
    next_json[p.ptr] = val;

    std::map<std::string, double> next_vals = current_vals;
    next_vals[p.name] = val;

    Generate(next_json, params, index + 1, next_vals, results);
  }
}

std::vector<RunConfig> GridScanner::GenerateGrid(const json& root) {
  std::vector<GridParam> params;

  // wrap the scan so any unexpected json type error reports the section
  // instead of bubbling up as a bare nlohmann exception.
  try {
    FindParams(root, "", params);
  } catch (const json::exception& e) {
    throw std::runtime_error("grid scan error: " + std::string(e.what()));
  }

  std::vector<RunConfig> results;
  std::map<std::string, double> initial_vals;

  // return single run if no grid params found
  if (params.empty()) {
    results.push_back({root, initial_vals});
    return results;
  }

  try {
    Generate(root, params, 0, initial_vals, results);
  } catch (const json::exception& e) {
    throw std::runtime_error("grid generation error: " + std::string(e.what()));
  }
  return results;
}

// ============================================================================
// FlattenGrids
// ============================================================================

namespace {

// a grid node is an object that carries min/max/step plus a scalar "val".
bool IsGridNode(const json& j) {
  return j.is_object() &&
         j.contains("min") && j["min"].is_number() &&
         j.contains("max") && j["max"].is_number() &&
         j.contains("step") && j["step"].is_number() &&
         j.contains("val") && j["val"].is_number();
}

void FlattenGridsRec(json& j) {
  if (j.is_object()) {
    if (IsGridNode(j)) {
      // collapse the whole node into its scalar "val"
      j = j["val"];
      return;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
      FlattenGridsRec(it.value());
    }
  } else if (j.is_array()) {
    for (auto& item : j) {
      FlattenGridsRec(item);
    }
  }
}

} // namespace

void FlattenGrids(json& j) {
  FlattenGridsRec(j);
}

// ============================================================================
// AppConfig::Validate
// ============================================================================

void AppConfig::Validate() const {
  auto fail = [](const std::string& msg) {
    throw std::invalid_argument("AppConfig validation failed: " + msg);
  };

  // --- environment ---
  if (interval_ms <= 0) {
    fail("environment.interval_ms must be > 0 (got " + std::to_string(interval_ms) + ")");
  }
  if (macro_interval_ms <= 0) {
    fail("environment.macro_interval_ms must be > 0 (got " + std::to_string(macro_interval_ms) + ")");
  }
  if (macro_interval_ms < interval_ms) {
    fail("environment.macro_interval_ms (" + std::to_string(macro_interval_ms) +
         ") must be >= environment.interval_ms (" + std::to_string(interval_ms) + ")");
  }
  if (tick_size <= 0.0) {
    fail("environment.tick_size must be > 0 (got " + std::to_string(tick_size) + ")");
  }
  if (vwap_reset_hour < 0 || vwap_reset_hour > 23) {
    fail("environment.vwap_reset_hour must be in [0, 23] (got " + std::to_string(vwap_reset_hour) + ")");
  }

  // --- execution ---
  if (taker_fee_pct < 0.0 || taker_fee_pct >= 0.1) {
    fail("execution.taker_fee_pct must be in [0.0, 0.1) (got " + std::to_string(taker_fee_pct) + ")");
  }
  if (slippage_pct < 0.0 || slippage_pct >= 0.1) {
    fail("execution.slippage_pct must be in [0.0, 0.1) (got " + std::to_string(slippage_pct) + ")");
  }

  // --- risk ---
  if (sl_pct <= 0.0 || sl_pct >= 0.1) {
    fail("risk.sl_pct must be in (0.0, 0.1) (got " + std::to_string(sl_pct) + ")");
  }
  if (tp_pct <= 0.0 || tp_pct >= 0.1) {
    fail("risk.tp_pct must be in (0.0, 0.1) (got " + std::to_string(tp_pct) + ")");
  }
  if (risk_per_trade_pct <= 0.0 || risk_per_trade_pct >= 0.1) {
    fail("risk.risk_per_trade_pct must be in (0.0, 0.1) (got " + std::to_string(risk_per_trade_pct) + ")");
  }
  if (min_distance_dollars < 0.0) {
    fail("risk.min_distance_dollars must be >= 0.0 (got " + std::to_string(min_distance_dollars) + ")");
  }
  if (max_daily_loss <= 0.0) {
    fail("risk.max_daily_loss must be > 0.0 (got " + std::to_string(max_daily_loss) + ")");
  }

  // --- trade management ---
  if (tm_config.be_trigger_pct <= 0.0 || tm_config.be_trigger_pct >= 0.1) {
    fail("trade_management.break_even.trigger_pct must be in (0.0, 0.1) (got " +
         std::to_string(tm_config.be_trigger_pct) + ")");
  }
  if (tm_config.be_target_pct < 0.0 || tm_config.be_target_pct >= 0.1) {
    fail("trade_management.break_even.target_pct must be in [0.0, 0.1) (got " +
         std::to_string(tm_config.be_target_pct) + ")");
  }
  if (tm_config.trailing_trigger_pct <= 0.0 || tm_config.trailing_trigger_pct >= 0.1) {
    fail("trade_management.trailing.trigger_pct must be in (0.0, 0.1) (got " +
         std::to_string(tm_config.trailing_trigger_pct) + ")");
  }
  if (tm_config.trailing_dist_pct <= 0.0 || tm_config.trailing_dist_pct >= 0.1) {
    fail("trade_management.trailing.distance_pct must be in (0.0, 0.1) (got " +
         std::to_string(tm_config.trailing_dist_pct) + ")");
  }
  if (tm_config.trailing_dist_pct >= tm_config.trailing_trigger_pct) {
    fail("trade_management.trailing.distance_pct (" + std::to_string(tm_config.trailing_dist_pct) +
         ") must be < trade_management.trailing.trigger_pct (" +
         std::to_string(tm_config.trailing_trigger_pct) + ")");
  }
  if (tm_config.scale_out_trigger_pct <= 0.0 || tm_config.scale_out_trigger_pct >= 0.1) {
    fail("trade_management.scale_out.trigger_pct must be in (0.0, 0.1) (got " +
         std::to_string(tm_config.scale_out_trigger_pct) + ")");
  }
  if (tm_config.scale_out_fraction < 0.1 || tm_config.scale_out_fraction > 0.9) {
    fail("trade_management.scale_out.fraction must be in [0.1, 0.9] (got " +
         std::to_string(tm_config.scale_out_fraction) + ")");
  }

  // --- components ---
  if (triggers.empty()) {
    fail("triggers must not be empty (at least one trigger is required)");
  }
  for (const auto& t : triggers) {
    if (t.name.empty()) {
      fail("triggers contains an entry with an empty name");
    }
  }
  for (const auto& f : filters) {
    if (f.name.empty()) {
      fail("filters contains an entry with an empty name");
    }
  }
  for (const auto& r : risk_modules) {
    if (r.name.empty()) {
      fail("risk.modules contains an entry with an empty name");
    }
  }
}

// ============================================================================
// AppConfig::Load
// ============================================================================

namespace {

// safely read a numeric field. if the value is an object (e.g. a grid
// definition with min/max/step) or missing, the default is returned instead
// of throwing. this keeps the parser tolerant of grid-style configs.
double SafeNumber(const json& parent, const std::string& key, double fallback) {
  if (!parent.contains(key)) return fallback;
  const auto& v = parent[key];
  if (v.is_number()) return v.get<double>();
  return fallback;
}

int SafeInt(const json& parent, const std::string& key, int fallback) {
  if (!parent.contains(key)) return fallback;
  const auto& v = parent[key];
  if (v.is_number_integer()) return v.get<int>();
  if (v.is_number()) return static_cast<int>(v.get<double>());
  return fallback;
}

bool SafeBool(const json& parent, const std::string& key, bool fallback) {
  if (!parent.contains(key)) return fallback;
  const auto& v = parent[key];
  if (v.is_boolean()) return v.get<bool>();
  return fallback;
}

std::string SafeString(const json& parent, const std::string& key, const std::string& fallback) {
  if (!parent.contains(key)) return fallback;
  const auto& v = parent[key];
  if (v.is_string()) return v.get<std::string>();
  return fallback;
}

// wrap a section parse in a try/catch that reports the section name.
template <typename Fn>
void ParseSection(const std::string& section, Fn&& fn) {
  try {
    fn();
  } catch (const json::exception& e) {
    throw std::runtime_error("config parse error in section '" + section + "': " + e.what());
  }
}

} // namespace

AppConfig AppConfig::Load(const json& j) {
  AppConfig cfg;

  // --- environment ---
  ParseSection("environment", [&]() {
    if (j.contains("environment")) {
      const auto& env = j["environment"];
      cfg.filepath = SafeString(env, "filepath", "");
      cfg.interval_ms = SafeInt(env, "interval_ms", 60000);
      cfg.macro_interval_ms = SafeInt(env, "macro_interval_ms", 900000);
      cfg.tick_size = SafeNumber(env, "tick_size", 1.0);
      cfg.vwap_reset_hour = SafeInt(env, "vwap_reset_hour", 0);

      if (env.contains("max_cores") && env["max_cores"].is_number_integer() &&
          env["max_cores"].get<int>() > 0) {
        cfg.max_cores = env["max_cores"].get<unsigned int>();
      } else {
        cfg.max_cores = std::thread::hardware_concurrency();
        if (cfg.max_cores == 0) cfg.max_cores = 4;
      }
    }
  });

  // --- exchange ---
  // read the data source ("binance" or "bybit") from the config. accepts the
  // key either at the top level or nested under "environment".
  ParseSection("exchange", [&]() {
    if (j.contains("exchange")) {
      cfg.exchange = SafeString(j, "exchange", cfg.exchange);
    } else if (j.contains("environment") && j["environment"].is_object()) {
      cfg.exchange = SafeString(j["environment"], "exchange", cfg.exchange);
    }
  });

  // --- execution ---
  ParseSection("execution", [&]() {
    if (j.contains("execution")) {
      const auto& ex = j["execution"];
      cfg.taker_fee_pct = SafeNumber(ex, "taker_fee_pct", 0.0004);
      cfg.slippage_pct = SafeNumber(ex, "slippage_pct", 0.0002);
    } else if (j.contains("environment")) {
      // fallback for backwards compatibility
      const auto& env = j["environment"];
      cfg.taker_fee_pct = SafeNumber(env, "taker_fee_pct", 0.0004);
      cfg.slippage_pct = SafeNumber(env, "slippage_pct", 0.0002);
    }
  });

  // --- triggers ---
  ParseSection("triggers", [&]() {
    const json* arr = nullptr;
    if (j.contains("triggers")) {
      arr = &j["triggers"];
    } else if (j.contains("strategy") && j["strategy"].contains("triggers")) {
      arr = &j["strategy"]["triggers"];
    }
    if (arr && arr->is_array()) {
      for (const auto& item : *arr) {
        if (!item.is_object()) continue;
        cfg.triggers.push_back({SafeString(item, "name", ""), item});
      }
    }
  });

  // --- filters ---
  ParseSection("filters", [&]() {
    const json* arr = nullptr;
    if (j.contains("filters")) {
      arr = &j["filters"];
    } else if (j.contains("strategy") && j["strategy"].contains("filters")) {
      arr = &j["strategy"]["filters"];
    }
    if (arr && arr->is_array()) {
      for (const auto& item : *arr) {
        if (!item.is_object()) continue;
        cfg.filters.push_back({SafeString(item, "name", ""), item});
      }
    }
  });

  // --- risk ---
  ParseSection("risk", [&]() {
    if (j.contains("risk")) {
      const auto& risk = j["risk"];
      // accept both canonical and legacy key names
      cfg.sl_pct = SafeNumber(risk, "sl_pct", SafeNumber(risk, "sl_price_pct", 0.002));
      cfg.tp_pct = SafeNumber(risk, "tp_pct", 0.01);
      cfg.risk_per_trade_pct = SafeNumber(risk, "risk_per_trade_pct", 0.01);
      cfg.min_distance_dollars = SafeNumber(risk, "min_distance_dollars", 10.0);
      cfg.max_daily_loss = SafeNumber(risk, "max_daily_loss", 400.0);

      if (risk.contains("modules") && risk["modules"].is_array()) {
        for (const auto& item : risk["modules"]) {
          if (!item.is_object()) continue;
          cfg.risk_modules.push_back({SafeString(item, "name", ""), item});
        }
      }
    }
  });

  // --- trade_management ---
  ParseSection("trade_management", [&]() {
    if (j.contains("trade_management")) {
      const auto& tm = j["trade_management"];
      if (tm.contains("break_even") && tm["break_even"].is_object()) {
        const auto& be = tm["break_even"];
        cfg.tm_config.be_trigger_pct = SafeNumber(be, "trigger_pct", 0.005);
        cfg.tm_config.be_target_pct = SafeNumber(be, "target_pct", 0.001);
      }
      if (tm.contains("trailing") && tm["trailing"].is_object()) {
        const auto& tr = tm["trailing"];
        cfg.tm_config.enable_trailing = SafeBool(tr, "enabled", true);
        cfg.tm_config.trailing_trigger_pct = SafeNumber(tr, "trigger_pct", 0.008);
        cfg.tm_config.trailing_dist_pct = SafeNumber(tr, "distance_pct", 0.004);
      }
      if (tm.contains("scale_out") && tm["scale_out"].is_object()) {
        const auto& so = tm["scale_out"];
        cfg.tm_config.enable_scale_out = SafeBool(so, "enabled", false);
        cfg.tm_config.scale_out_trigger_pct = SafeNumber(so, "trigger_pct", 0.006);
        cfg.tm_config.scale_out_fraction = SafeNumber(so, "fraction", 0.5);
      }
    }
  });

  // fail fast: abort before any EngineInstance is built
  cfg.Validate();

  return cfg;
}
