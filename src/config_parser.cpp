#include "config_parser.hpp"
#include "trade_management.hpp"

#include <cctype>
#include <cmath>
#include <initializer_list>
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
      // the leaf name is what the csv/report shows. two grid nodes with the
      // same leaf (e.g. trigger_r under break_even and trailing) would
      // collapse into one map entry and one results.csv column, silently
      // hiding one of the swept values. qualify both by their json path as
      // soon as a collision is detected.
      auto path_name = [](const std::string& p) {
        return (!p.empty() && p.front() == '/') ? p.substr(1) : p;
      };
      std::string param_name = current_path.substr(current_path.find_last_of('/') + 1);
      for (auto& existing : params) {
        if (existing.name == param_name) {
          existing.name = path_name(existing.ptr.to_string());
          param_name = path_name(current_path);
          break;
        }
      }
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
  const double step = (p.step == 0.0) ? 1.0 : p.step;

  // validate the sweep before emitting anything: min > max silently produced
  // zero permutations, which surfaced as "SCAN COMPLETE" with no runs.
  if (!std::isfinite(p.min) || !std::isfinite(p.max) ||
      !std::isfinite(step) || !(p.min <= p.max) || !(step > 0.0)) {
    throw std::runtime_error(
        "grid error: invalid range for parameter '" + p.name + "' (min=" +
        std::to_string(p.min) + " max=" + std::to_string(p.max) +
        " step=" + std::to_string(p.step) + ")");
  }

  // iterate over an integer count instead of accumulating with +=. floating
  // accumulation drifts: it either emits one value past max or stops short of
  // it, so the tuned last value is never actually tested.
  const long long steps = static_cast<long long>(
      std::floor((p.max - p.min) / step + 1e-9));

  // create permutations for current param
  for (long long k = 0; k <= steps; ++k) {
    const double val = p.min + static_cast<double>(k) * step;
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

  if (results.empty()) {
    throw std::runtime_error("grid generation produced no runs");
  }
  return results;
}

// ============================================================================
// FlattenGrids
// ============================================================================

namespace {

// true when j["val"] is a number or a numeric string. hand-written configs
// carry "val": "0.5"; leaving that as an object meant the live factory saw a
// grid node as a param object and every numeric reader fell back to default.
bool HasScalarVal(const json& j) {
  if (!j.contains("val")) return false;
  const auto& v = j["val"];
  if (v.is_number()) return true;
  if (!v.is_string()) return false;
  try {
    const std::string s = v.get<std::string>();
    size_t used = 0;
    std::stod(s, &used);
    while (used < s.size() && std::isspace(static_cast<unsigned char>(s[used]))) {
      ++used;
    }
    return used == s.size();
  } catch (...) {
    return false;
  }
}

// a grid node is an object that carries min/max/step plus a scalar "val".
bool IsGridNode(const json& j) {
  return j.is_object() &&
         j.contains("min") && j["min"].is_number() &&
         j.contains("max") && j["max"].is_number() &&
         j.contains("step") && j["step"].is_number() &&
         HasScalarVal(j);
}

void FlattenGridsRec(json& j) {
  if (j.is_object()) {
    if (IsGridNode(j)) {
      // collapse the whole node into its scalar "val". keep the original
      // type: SafeNumber and the factory both parse numeric strings.
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

  // --- auth ---
  // live venues require real credentials. without this check a blank key only
  // surfaces as a signed-request failure (Bybit retCode 10003) at the first
  // order, long after startup.
  if (exchange == "bybit") {
    if (api_key.empty()) {
      fail("api_key must not be empty when exchange is \"bybit\"");
    }
    if (api_secret.empty()) {
      fail("api_secret must not be empty when exchange is \"bybit\"");
    }
  }

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
  if (min_distance_dollars < 0.0 || !std::isfinite(min_distance_dollars)) {
    fail("risk.min_distance_dollars must be >= 0.0 in quote-currency units "
         "(0 disables the widening) (got " + std::to_string(min_distance_dollars) + ")");
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
  // only meaningful when trailing is on: a disabled block with stale values
  // aborted the whole config for a feature that will never run.
  if (tm_config.enable_trailing &&
      tm_config.trailing_dist_pct >= tm_config.trailing_trigger_pct) {
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
//
// numeric strings are accepted too: hand-written configs carry "0.004"
// rather than 0.004, and silently falling back to the default there changes
// the risk of the run without any error.
double SafeNumber(const json& parent, const std::string& key, double fallback) {
  if (!parent.contains(key)) return fallback;
  const auto& v = parent[key];
  if (v.is_number()) return v.get<double>();
  if (v.is_string()) {
    try {
      const std::string s = v.get<std::string>();
      size_t used = 0;
      const double parsed = std::stod(s, &used);
      while (used < s.size() && std::isspace(static_cast<unsigned char>(s[used]))) {
        ++used;
      }
      if (used == s.size() && std::isfinite(parsed)) return parsed;
    } catch (...) {
      // fall through to the default
    }
  }
  return fallback;
}


// read a boolean field. accepts the same spellings GridScanner treats as a
// disable flag: bool, "true"/"false"/"1"/"0", and integer 1/0. without this,
// {"enabled": "false"} silently returned the fallback (true for trailing),
// enabling a component the config explicitly turned off.
bool SafeBool(const json& parent, const std::string& key, bool fallback) {
  if (!parent.contains(key)) return fallback;
  const auto& v = parent[key];
  if (v.is_boolean()) return v.get<bool>();
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    if (s == "true" || s == "True" || s == "TRUE" || s == "1") return true;
    if (s == "false" || s == "False" || s == "FALSE" || s == "0") return false;
    return fallback;
  }
  if (v.is_number_integer()) return v.get<long long>() != 0;
  if (v.is_number_float()) return v.get<double>() != 0.0;
  return fallback;
}

std::string SafeString(const json& parent, const std::string& key, const std::string& fallback) {
  if (!parent.contains(key)) return fallback;
  const auto& v = parent[key];
  if (v.is_string()) return v.get<std::string>();
  return fallback;
}

// strict readers for every field that changes what a run does. a missing or
// mistyped key aborts with the full path instead of substituting a number the
// author never wrote: a run must mean exactly what its config says.
double RequireNumber(const json& parent, const std::string& key,
                     const std::string& section) {
  const std::string path = section + "." + key;
  if (!parent.contains(key)) {
    throw std::runtime_error("config error: missing required key '" + path + "'");
  }
  const auto& v = parent[key];
  if (v.is_number()) {
    const double d = v.get<double>();
    if (!std::isfinite(d)) {
      throw std::runtime_error("config error: '" + path + "' must be finite");
    }
    return d;
  }
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    size_t used = 0;
    try {
      const double parsed = std::stod(s, &used);
      while (used < s.size() && std::isspace(static_cast<unsigned char>(s[used]))) {
        ++used;
      }
      if (used == s.size() && std::isfinite(parsed)) return parsed;
    } catch (...) {
      // fall through to the typed error below
    }
  }
  throw std::runtime_error("config error: '" + path + "' must be a number (got " +
                           v.dump() + ")");
}

int RequireInt(const json& parent, const std::string& key,
               const std::string& section) {
  const std::string path = section + "." + key;
  if (!parent.contains(key)) {
    throw std::runtime_error("config error: missing required key '" + path + "'");
  }
  const auto& v = parent[key];
  if (v.is_number_integer()) return v.get<int>();
  if (v.is_number_float()) {
    const double d = v.get<double>();
    if (std::floor(d) == d) return static_cast<int>(d);
  }
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    size_t used = 0;
    try {
      const long long parsed = std::stoll(s, &used);
      while (used < s.size() && std::isspace(static_cast<unsigned char>(s[used]))) {
        ++used;
      }
      if (used == s.size()) return static_cast<int>(parsed);
    } catch (...) {
      // fall through
    }
  }
  throw std::runtime_error("config error: '" + path + "' must be an integer (got " +
                           v.dump() + ")");
}

bool RequireBool(const json& parent, const std::string& key,
                 const std::string& section) {
  const std::string path = section + "." + key;
  if (!parent.contains(key)) {
    throw std::runtime_error("config error: missing required key '" + path + "'");
  }
  const auto& v = parent[key];
  if (v.is_boolean()) return v.get<bool>();
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    if (s == "true" || s == "True" || s == "TRUE" || s == "1") return true;
    if (s == "false" || s == "False" || s == "FALSE" || s == "0") return false;
  }
  if (v.is_number_integer()) return v.get<long long>() != 0;
  if (v.is_number_float()) return v.get<double>() != 0.0;
  throw std::runtime_error("config error: '" + path + "' must be a boolean (got " +
                           v.dump() + ")");
}

double RequireNumberAny(const json& parent, std::initializer_list<const char*> keys,
                        const std::string& section) {
  for (const char* k : keys) {
    if (parent.contains(k)) return RequireNumber(parent, k, section);
  }
  std::string names;
  for (const char* k : keys) {
    if (!names.empty()) names += " / ";
    names += std::string(section) + "." + k;
  }
  throw std::runtime_error("config error: one of '" + names + "' is required");
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

  // --- auth ---
  // credentials live at the root of the config, and may also be nested under
  // "exchange" or "environment" for configs that group connection settings:
  //   {"api_key": "...", "api_secret": "...", "is_testnet": true, ...}
  //   {"environment": {"api_key": "...", "api_secret": "...", ...}}
  ParseSection("auth", [&]() {
    // resolve the source once so all three fields are read from the same
    // place. root keys win; the nested lookup only applies when the root has
    // no credential keys at all.
    const json* creds = &j;
    if (!j.contains("api_key") && !j.contains("api_secret")) {
      if (j.contains("exchange") && j["exchange"].is_object()) {
        creds = &j["exchange"];
      } else if (j.contains("environment") && j["environment"].is_object()) {
        creds = &j["environment"];
      }
    }

    cfg.api_key = SafeString(j, "api_key", SafeString(*creds, "api_key", ""));
    cfg.api_secret = SafeString(j, "api_secret", SafeString(*creds, "api_secret", ""));
    // cfg.is_testnet already defaults to true, so it doubles as the fallback.
    cfg.is_testnet = SafeBool(j, "is_testnet", SafeBool(*creds, "is_testnet", cfg.is_testnet));
  });

  // --- environment ---
  ParseSection("environment", [&]() {
    if (!j.contains("environment") || !j["environment"].is_object()) {
      throw std::runtime_error("config error: missing required section 'environment'");
    }
    {
      const auto& env = j["environment"];
      cfg.filepath = SafeString(env, "filepath", "");
      cfg.interval_ms = RequireInt(env, "interval_ms", "environment");
      cfg.macro_interval_ms = RequireInt(env, "macro_interval_ms", "environment");
      cfg.tick_size = RequireNumber(env, "tick_size", "environment");
      cfg.vwap_reset_hour = RequireInt(env, "vwap_reset_hour", "environment");

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
    // never default the venue. a missing key used to mean "binance", which
    // would stream binance data for an account configured elsewhere.
    bool found = false;
    if (j.contains("exchange")) {
      const auto& ex = j["exchange"];
      if (ex.is_string()) {
        cfg.exchange = ex.get<std::string>();
        found = !cfg.exchange.empty();
      } else if (ex.is_object()) {
        // credentials may be grouped under "exchange"; the venue name then
        // lives inside that object.
        const std::string name = SafeString(ex, "name", SafeString(ex, "venue", ""));
        if (!name.empty()) {
          cfg.exchange = name;
          found = true;
        }
      }
    }
    if (!found && j.contains("environment") && j["environment"].is_object()) {
      const std::string name = SafeString(j["environment"], "exchange", "");
      if (!name.empty()) {
        cfg.exchange = name;
        found = true;
      }
    }
    if (!found) {
      throw std::runtime_error("config error: 'exchange' is required");
    }
  });

  // --- execution ---
  ParseSection("execution", [&]() {
    // fees and slippage change every pnl number in the report, so they are
    // never guessed. accepted at the root or, for older configs, inside
    // "environment".
    const json* ex = nullptr;
    std::string section;
    if (j.contains("execution") && j["execution"].is_object()) {
      ex = &j["execution"];
      section = "execution";
    } else if (j.contains("environment") && j["environment"].is_object()) {
      ex = &j["environment"];
      section = "environment";
    }
    if (!ex) {
      throw std::runtime_error("config error: missing required section 'execution'");
    }
    cfg.taker_fee_pct = RequireNumber(*ex, "taker_fee_pct", section);
    cfg.slippage_pct = RequireNumber(*ex, "slippage_pct", section);
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
    if (!j.contains("risk") || !j["risk"].is_object()) {
      throw std::runtime_error("config error: missing required section 'risk'");
    }
    {
      const auto& risk = j["risk"];
      // 1R is defined by sl_pct and nothing else.
      cfg.sl_pct = RequireNumberAny(risk, {"sl_pct", "sl_price_pct"}, "risk");
      // tp_r (a multiple of sl_pct, i.e. of 1R) wins over the legacy absolute
      // tp_pct. a config that names neither fails to load instead of keeping
      // a 1% guess.
      cfg.tp_pct = mokuren::ResolveTpPct(j, cfg.sl_pct);
      cfg.risk_per_trade_pct = RequireNumber(risk, "risk_per_trade_pct", "risk");
      // optional by design: 0 (the default here) means "never widen the
      // strategy's own stop", so absence has a defined, non-arbitrary meaning.
      cfg.min_distance_dollars = SafeNumber(risk, "min_distance_dollars", 0.0);
      cfg.max_daily_loss = RequireNumber(risk, "max_daily_loss", "risk");

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
    if (!j.contains("trade_management") || !j["trade_management"].is_object()) {
      throw std::runtime_error(
          "config error: missing required section 'trade_management'");
    }
    {
      const auto& tm = j["trade_management"];
      // every level is required, in R or via the legacy absolute key. sl_pct
      // is 1R, so one config means the same thing in every path.
      if (!tm.contains("break_even") || !tm["break_even"].is_object()) {
        throw std::runtime_error(
            "config error: missing required section 'trade_management.break_even'");
      }
      const auto& be = tm["break_even"];
      cfg.tm_config.be_trigger_pct = mokuren::ResolveLevelPct(
          be, cfg.sl_pct, "trigger_r", "trigger_pct", "trade_management.break_even");
      cfg.tm_config.be_target_pct = mokuren::ResolveLevelPct(
          be, cfg.sl_pct, "target_r", "target_pct", "trade_management.break_even");
      // only the arming decision follows the flag; the levels above stay
      // required so a config means the same thing whether or not the block is
      // switched on. absent => enabled, matching ApplyRMultiplesManagement.
      cfg.tm_config.enable_break_even = SafeBool(be, "enabled", true);

      if (!tm.contains("trailing") || !tm["trailing"].is_object()) {
        throw std::runtime_error(
            "config error: missing required section 'trade_management.trailing'");
      }
      const auto& tr = tm["trailing"];
      cfg.tm_config.enable_trailing =
          RequireBool(tr, "enabled", "trade_management.trailing");
      cfg.tm_config.trailing_trigger_pct = mokuren::ResolveLevelPct(
          tr, cfg.sl_pct, "trigger_r", "trigger_pct", "trade_management.trailing");
      cfg.tm_config.trailing_dist_pct = mokuren::ResolveLevelPct(
          tr, cfg.sl_pct, "distance_r", "distance_pct", "trade_management.trailing");

      if (!tm.contains("scale_out") || !tm["scale_out"].is_object()) {
        throw std::runtime_error(
            "config error: missing required section 'trade_management.scale_out'");
      }
      const auto& so = tm["scale_out"];
      cfg.tm_config.enable_scale_out =
          RequireBool(so, "enabled", "trade_management.scale_out");
      cfg.tm_config.scale_out_trigger_pct = mokuren::ResolveLevelPct(
          so, cfg.sl_pct, "trigger_r", "trigger_pct", "trade_management.scale_out");
      cfg.tm_config.scale_out_fraction =
          RequireNumber(so, "fraction", "trade_management.scale_out");
    }
  });

  // fail fast: abort before any EngineInstance is built
  cfg.Validate();

  return cfg;
}
