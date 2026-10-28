#include "config_parser.hpp"
#include "trade_management.hpp"

#include <cctype>
#include <cmath>
#include <cstdint>
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

namespace {
// defined with the other config readers further down; declared here because
// Validate rejects an all-disabled pipeline and needs it first.
bool SafeBool(const json& parent, const std::string& key, bool fallback);
} // namespace

// ============================================================================
// AppConfig::Validate
// ============================================================================

void AppConfig::Validate(bool require_venue_credentials) const {
  auto fail = [](const std::string& msg) {
    throw std::invalid_argument("AppConfig validation failed: " + msg);
  };

  // --- auth ---
  // live venues require real credentials. without this check a blank key only
  // surfaces as a signed-request failure (Bybit retCode 10003) at the first
  // order, long after startup.
  if (require_venue_credentials && exchange == "bybit") {
    if (api_key.empty()) {
      fail("api_key must not be empty when exchange is \"bybit\"");
    }
    if (api_secret.empty()) {
      fail("api_secret must not be empty when exchange is \"bybit\"");
    }
  }

  // --- venue ---
  // whitelist the venue name. a typo used to skip the credential check above
  // and then fall through the factory's string comparison into a silent
  // paper-trading run.
  //
  // binance is deliberately absent. its spot bookTicker feed is depth-1 only
  // (and the futures stream, the one with real depth, is not available to this
  // deployment), so it cannot satisfy the strategy's depth triggers -- and
  // worse, the startup gate cannot catch it: one level per side is a valid
  // L2Snapshot, so the preflight would report "l2 ok" while
  // StackedImbalance/SpoofHunter read zeroed level-2 slots. listing a venue
  // whose feed can pass the checks but cannot feed the alpha is worse than not
  // listing it at all.
  if (exchange != "bybit" && exchange != "hyperliquid") {
    fail("exchange must be one of 'bybit', 'hyperliquid' (got '" +
         exchange + "')");
  }
  if (category != "linear" && category != "spot" && category != "inverse") {
    fail("venue.category must be one of 'linear', 'spot', 'inverse' (got '" +
         category + "')");
  }
  if (symbol.empty()) {
    fail("venue.symbol must not be empty");
  }
  if (position_mode != "one_way" && position_mode != "hedge") {
    fail("venue.position_mode must be 'one_way' or 'hedge' (got '" +
         position_mode + "')");
  }
  // gates the EXECUTION world only: a mainnet feed signs nothing and places
  // nothing, so mainnet data into a demo account is not a funded run.
  if (exchange == "bybit" && env == mokuren::venue::Env::Mainnet &&
      !allow_mainnet) {
    fail("venue environment 'mainnet' requires allow_mainnet: true (this is "
         "the EXECUTION world; to read mainnet data into a demo/testnet "
         "account, point venue.environment at the account and set "
         "venue.data_environment to 'mainnet')");
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
  // a zero step divides the size to infinity; a negative one rounds it to a
  // negative quantity. both are orders no venue should ever see.
  if (!std::isfinite(lot_step) || lot_step <= 0.0) {
    fail("venue.lot_step must be a finite number > 0 (got " +
         std::to_string(lot_step) + ")");
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
  // tp_pct is derived when the config uses tp_r (tp_pct == tp_r * sl_pct), so
  // the old absolute cap of 0.1 rejected valid R multiples the moment a grid
  // swept sl_pct upward -- and the failure named a key the operator never
  // wrote. a 10R target at 1R = 0.02 is 20% by definition, not an error.
  //
  // bound the R multiple where one exists; keep only a generous absolute
  // ceiling on the fraction, and only to catch unit mistakes (4.0 written for
  // 0.04). the ceiling is deliberately wide enough that no sane R sits under
  // it, because the R is the real limit.
  if (tp_r > 0.0 && (!std::isfinite(tp_r) || tp_r > 100.0)) {
    fail("risk.tp_r must be a finite multiple of 1R in (0.0, 100.0] (got " +
         std::to_string(tp_r) + ")");
  }
  // widen only where the value is DERIVED. when the config wrote an absolute
  // tp_pct nothing scales it, and the old 0.1 cap was right: 0.3 means 30% of
  // price, which is a unit mistake, not a target. when tp_pct == tp_r *
  // sl_pct, growth is legitimate by definition and only a wide ceiling
  // applies.
  const double tp_ceiling = tp_r > 0.0 ? 0.5 : 0.1;
  if (!std::isfinite(tp_pct) || tp_pct <= 0.0 || tp_pct >= tp_ceiling) {
    std::string msg = "risk.tp_pct must be in (0.0, " +
                      std::to_string(tp_ceiling) + ") (got " +
                      std::to_string(tp_pct);
    if (tp_r > 0.0) {
      // name the derivation: "tp_pct" is the wrong thing to go edit here.
      msg += ", derived from risk.tp_r " + std::to_string(tp_r) +
             " x risk.sl_pct " + std::to_string(sl_pct) +
             " -- lower tp_r or narrow the sl_pct sweep";
    }
    fail(msg + ")");
  }
  if (risk_per_trade_pct <= 0.0 || risk_per_trade_pct >= 0.1) {
    fail("risk.risk_per_trade_pct must be in (0.0, 0.1) (got " + std::to_string(risk_per_trade_pct) + ")");
  }
  if (min_distance_dollars < 0.0 || !std::isfinite(min_distance_dollars)) {
    fail("risk.min_distance_dollars must be >= 0.0 in quote-currency units "
         "(0 disables the widening) (got " + std::to_string(min_distance_dollars) + ")");
  }
  // starting capital is the denominator of every drawdown figure
  // (PaperTrader::UpdateDrawdown divides by peak_balance_). a zero or negative
  // seed produced NaN drawdowns instead of an error.
  if (!(initial_balance > 0.0) || !std::isfinite(initial_balance)) {
    fail("initial_balance must be a finite number > 0 (got " +
         std::to_string(initial_balance) + ")");
  }

  // --- trade management ---
  // every level here is derived too: ResolveLevelPct multiplies the canonical
  // *_r key by sl_pct (1R). the old absolute cap of 0.1 therefore rejected
  // configurations whose R multiples are perfectly ordinary as soon as sl_pct
  // itself was swept -- a 2R break-even at 1R = 0.06 is 12%. Same fix as
  // risk.tp_pct: a wide absolute ceiling that only catches unit errors, with
  // the relative relationships (distance < trigger) still enforced below.
  if (tm_config.be_trigger_pct <= 0.0 || tm_config.be_trigger_pct >= 0.5) {
    fail("trade_management.break_even.trigger_pct must be in (0.0, 0.5) (got " +
         std::to_string(tm_config.be_trigger_pct) +
         " = trigger_r x risk.sl_pct)");
  }
  if (tm_config.be_target_pct < 0.0 || tm_config.be_target_pct >= 0.5) {
    fail("trade_management.break_even.target_pct must be in [0.0, 0.5) (got " +
         std::to_string(tm_config.be_target_pct) +
         " = target_r x risk.sl_pct)");
  }
  if (tm_config.trailing_trigger_pct <= 0.0 || tm_config.trailing_trigger_pct >= 0.5) {
    fail("trade_management.trailing.trigger_pct must be in (0.0, 0.5) (got " +
         std::to_string(tm_config.trailing_trigger_pct) +
         " = trigger_r x risk.sl_pct)");
  }
  if (tm_config.trailing_dist_pct <= 0.0 || tm_config.trailing_dist_pct >= 0.5) {
    fail("trade_management.trailing.distance_pct must be in (0.0, 0.5) (got " +
         std::to_string(tm_config.trailing_dist_pct) +
         " = distance_r x risk.sl_pct)");
  }
  // only meaningful when trailing is on: a disabled block with stale values
  // aborted the whole config for a feature that will never run.
  if (tm_config.enable_trailing &&
      tm_config.trailing_dist_pct >= tm_config.trailing_trigger_pct) {
    fail("trade_management.trailing.distance_pct (" + std::to_string(tm_config.trailing_dist_pct) +
         ") must be < trade_management.trailing.trigger_pct (" +
         std::to_string(tm_config.trailing_trigger_pct) + ")");
  }
  if (tm_config.scale_out_trigger_pct <= 0.0 || tm_config.scale_out_trigger_pct >= 0.5) {
    fail("trade_management.scale_out.trigger_pct must be in (0.0, 0.5) (got " +
         std::to_string(tm_config.scale_out_trigger_pct) +
         " = trigger_r x risk.sl_pct)");
  }
  if (tm_config.scale_out_fraction < 0.1 || tm_config.scale_out_fraction > 0.9) {
    fail("trade_management.scale_out.fraction must be in [0.1, 0.9] (got " +
         std::to_string(tm_config.scale_out_fraction) + ")");
  }

  // --- startup probe ---
  // a gate whose soak is longer than its own timeout can never pass: the
  // deadline fires first on every single run, and the failure reads as "the
  // venue is broken" rather than "those two numbers are impossible".
  if (probe.timeout_ms < probe.min_duration_ms) {
    fail("startup_probe.timeout_ms (" + std::to_string(probe.timeout_ms) +
         ") must be >= startup_probe.min_duration_ms (" +
         std::to_string(probe.min_duration_ms) + ")");
  }
  if (probe.timeout_ms < 0 || probe.min_duration_ms < 0) {
    fail("startup_probe.min_duration_ms and timeout_ms must be >= 0 ms");
  }
  // a requirement deeper than the engine can store can never be met: every
  // frame would be rejected as shallow, and the gate would report "the ven-
  // ue is broken" rather than "that number is impossible".
  if (probe.min_l2_depth > static_cast<uint64_t>(MAX_L2_DEPTH)) {
    fail("startup_probe.min_l2_depth (" +
         std::to_string(probe.min_l2_depth) +
         ") exceeds the maximum book depth this engine stores (" +
         std::to_string(MAX_L2_DEPTH) + ")");
  }

  // --- components ---
  if (triggers.empty()) {
    fail("triggers must not be empty (at least one trigger is required)");
  }
  bool any_trigger_active = false;
  for (const auto& t : triggers) {
    if (t.name.empty()) {
      fail("triggers contains an entry with an empty name");
    }
    // a missing flag means enabled, matching GridScanner::FindParams, which
    // prunes on all three. reading only "active" let {"enabled": false} pass
    // validation and then vanish from the sweep.
    const bool disabled = !SafeBool(t.params, "active", true) ||
                          !SafeBool(t.params, "enabled", true) ||
                          !SafeBool(t.params, "isActive", true);
    if (!disabled) any_trigger_active = true;
  }
  if (!any_trigger_active) {
    fail("every entry in 'triggers' is disabled -- the pipeline can never "
         "emit a signal, so every run yields zero trades (set active: true on "
         "at least one trigger)");
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
// AppConfig::DataSpec / ExecSpec
// ============================================================================

mokuren::venue::Spec AppConfig::DataSpec() const {
  // the instrument is shared; only the world differs. DataEnv() falls back to
  // env, so a config that names one environment resolves both specs to it.
  return mokuren::venue::Resolve(DataEnv(), category, symbol);
}

mokuren::venue::Spec AppConfig::ExecSpec() const {
  return mokuren::venue::Resolve(env, category, symbol);
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

AppConfig AppConfig::Load(const json& j, bool require_venue_credentials) {
  AppConfig cfg;

  // collapse any grid node ({"min":..,"max":..,"step":..,"val":..}) into its
  // scalar "val" before reading. the TUI seeds configs from _defaults.json,
  // which carries grid nodes on purpose so GridScanner can sweep them; the
  // normal run path (single backtest, --paper, --live) must see plain numbers
  // or every numeric reader falls back to its default. FlattenGrids is a
  // no-op on an already-flat config, so this is safe on every path.
  //
  // the backtest grid path (GridRunner/WfaRunner) calls GenerateGrid on the
  // ORIGINAL json first, then Load on each permutation's full_json -- which
  // Generate already wrote as scalars -- so flattening here does not disturb
  // the sweep.
  json flat = j;
  FlattenGrids(flat);
  const json& src = flat;

  // --- auth ---
  // credentials live at the root of the config, and may also be nested under
  // "exchange" or "environment" for configs that group connection settings:
  //   {"api_key": "...", "api_secret": "...", "is_testnet": true, ...}
  //   {"environment": {"api_key": "...", "api_secret": "...", ...}}
  ParseSection("auth", [&]() {
    // resolve the source once so all three fields are read from the same
    // place. root keys win; the nested lookup only applies when the root has
    // no credential keys at all.
    const json* creds = &src;
    if (!src.contains("api_key") && !src.contains("api_secret")) {
      if (src.contains("exchange") && src["exchange"].is_object()) {
        creds = &src["exchange"];
      } else if (src.contains("environment") && src["environment"].is_object()) {
        creds = &src["environment"];
      }
    }

    cfg.api_key = SafeString(src, "api_key", SafeString(*creds, "api_key", ""));
    cfg.api_secret = SafeString(src, "api_secret", SafeString(*creds, "api_secret", ""));
    // cfg.is_testnet already defaults to true, so it doubles as the fallback.
    cfg.is_testnet = SafeBool(src, "is_testnet", SafeBool(*creds, "is_testnet", cfg.is_testnet));
  });

  // --- paper account starting capital ---
  // only consumed in --paper (shadow) mode, where the factory hands this to
  // PaperTrader instead of the compile-time default. optional on purpose: a
  // config that omits it must behave exactly like today's paper runs.
  //
  // presence is captured here so the prop section below can refuse a
  // prop-enabled config that never named a base. the struct default stays
  // 10000 for directly-constructed AppConfig (tests, tooling); the check
  // belongs at load time, not at use time.
  const bool has_initial_balance = src.contains("initial_balance");
  cfg.initial_balance = SafeNumber(src, "initial_balance", cfg.initial_balance);

  // --- environment ---
  ParseSection("environment", [&]() {
    if (!src.contains("environment") || !src["environment"].is_object()) {
      throw std::runtime_error("config error: missing required section 'environment'");
    }
    {
      const auto& env = src["environment"];
      // `dataset` is canonical; `filepath` is a legacy alias kept so older
      // configs keep loading. one key, one owner -- the grid runner already
      // reads `dataset`, so accepting both under one name removes the
      // two-readers-one-file bug.
      cfg.filepath = SafeString(env, "dataset", SafeString(env, "filepath", ""));
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
  // read the data source ("bybit" or "hyperliquid") from the config. accepts the
  // key either at the top level or nested under "environment".
  ParseSection("exchange", [&]() {
    // never default the venue. a missing key used to mean "binance", which
    // would stream binance data for an account configured elsewhere.
    bool found = false;
    if (src.contains("exchange")) {
      const auto& ex = src["exchange"];
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
    if (!found && src.contains("environment") && src["environment"].is_object()) {
      const std::string name = SafeString(src["environment"], "exchange", "");
      if (!name.empty()) {
        cfg.exchange = name;
        found = true;
      }
    }
    if (!found) {
      throw std::runtime_error("config error: 'exchange' is required");
    }
  });

  // --- venue identity ---
  // environment + category + symbol + position mode form ONE identity. the
  // broker and the streamer are both built from it, and neither may derive a
  // host or a symbol of its own -- that is how a config ended up streaming
  // mainnet data into a testnet account.
  ParseSection("venue", [&]() {
    if (src.contains("venue") && src["venue"].is_object()) {
      const auto& v = src["venue"];
      const std::string env_name =
          SafeString(v, "environment", SafeString(v, "env", ""));
      if (!env_name.empty()) {
        try {
          cfg.env = mokuren::venue::ParseEnv(env_name);
        } catch (const std::exception& e) {
          throw std::runtime_error(std::string("config error: venue.environment: ") +
                                   e.what());
        }
        cfg.is_testnet = (cfg.env != mokuren::venue::Env::Mainnet);
      }
      cfg.category = SafeString(v, "category", cfg.category);
      cfg.symbol = SafeString(v, "symbol", cfg.symbol);
      cfg.position_mode = SafeString(v, "position_mode", cfg.position_mode);
      // order quantity grid. optional, but when present it must be a real
      // increment: it is a per-symbol venue constraint, not a tuning knob.
      cfg.lot_step = SafeNumber(v, "lot_step", cfg.lot_step);
      cfg.allow_mainnet = SafeBool(v, "allow_mainnet", cfg.allow_mainnet);
    } else if (src.contains("is_testnet")) {
      // legacy spelling. demo cannot be selected this way, on purpose.
      cfg.is_testnet = SafeBool(src, "is_testnet", true);
      cfg.env = cfg.is_testnet ? mokuren::venue::Env::Testnet
                               : mokuren::venue::Env::Mainnet;
    }

    // --- market-data world ------------------------------------------------
    // optional, and the only part of the venue identity that may differ from
    // the account: a key is bound to one world, the public book is free. the
    // instrument stays shared -- a feed for a different symbol would size a
    // contract the engine never priced.
    //
    // accepted spellings, most specific first:
    //   "venue":      {"data_environment": "mainnet"}
    //   "venue":      {"data_venue": {"environment": "mainnet"}}
    //   "data_venue": {"environment": "mainnet"}                 (root)
    //
    // absent is the default and means "the feed and the account are the same
    // world", so a config written before the split keeps its exact meaning.
    {
      std::string data_env_name;
      auto read_block = [](const json& block) -> std::string {
        if (!block.is_object()) return std::string();
        return SafeString(block, "environment", SafeString(block, "env", ""));
      };
      if (src.contains("venue") && src["venue"].is_object()) {
        const auto& v = src["venue"];
        data_env_name = SafeString(v, "data_environment", "");
        if (data_env_name.empty() && v.contains("data_venue")) {
          data_env_name = read_block(v["data_venue"]);
        }
      }
      if (data_env_name.empty() && src.contains("data_venue")) {
        data_env_name = read_block(src["data_venue"]);
      }
      if (!data_env_name.empty()) {
        try {
          cfg.data_env = mokuren::venue::ParseEnv(data_env_name);
        } catch (const std::exception& e) {
          throw std::runtime_error(
              std::string("config error: venue.data_environment: ") + e.what());
        }
      }
    }

    // root-level overrides for the instrument identity.
    cfg.category = SafeString(src, "category", cfg.category);
    cfg.symbol = SafeString(src, "symbol", cfg.symbol);
    cfg.allow_mainnet = SafeBool(src, "allow_mainnet", cfg.allow_mainnet);
  });

  // --- funded-account survival rules ---
  // parsed as their own section and deliberately not under a strategy block:
  // these limits belong to the account, not to a parameter set. a fraction
  // (0.04) is the unit here, matching every other pct in this file.
  if (src.contains("prop") && src["prop"].is_object()) {
    const auto& p = src["prop"];

    // numbers or numeric strings, as the rest of this loader accepts.
    auto read_num = [&p](const char* key, double fallback) {
      if (!p.contains(key)) return fallback;
      const auto& v = p[key];
      if (v.is_number()) return v.get<double>();
      if (v.is_string()) {
        try {
          return std::stod(v.get<std::string>());
        } catch (...) {
          return fallback;
        }
      }
      return fallback;
    };

    // presence of the block IS the opt-in. a config that writes
    // "prop": { ... } means "measure this account", and the old fallback
    // (false) made a block whose author omitted one key behave exactly like no
    // block at all: no daily limit, no total limit, in a backtest that looked
    // configured. an explicit "enabled": false still turns it off, and that is
    // now the only way to say it.
    cfg.prop.enabled = SafeBool(p, "enabled", true);
    cfg.prop.max_daily_drawdown_pct =
        read_num("max_daily_drawdown_pct", cfg.prop.max_daily_drawdown_pct);
    cfg.prop.max_total_drawdown_pct =
        read_num("max_total_drawdown_pct", cfg.prop.max_total_drawdown_pct);
    cfg.prop.warn_fraction = read_num("warn_fraction", cfg.prop.warn_fraction);
    // a starting balance is account state and is passed to PropGuard by the
    // caller (cfg.initial_balance on the backtest paths, the firm's baseline
    // live). accepting it here would reintroduce a second, silently different
    // base -- the bug that made total drawdown report against an account the
    // run never funded -- so name it instead of quietly ignoring it.
    if (p.contains("initial_balance")) {
      throw std::runtime_error(
          "config error: prop.initial_balance is no longer read; total "
          "drawdown is measured against initial_balance (the account seed)");
    }
    cfg.prop.flatten_on_breach =
        SafeBool(p, "flatten_on_breach", cfg.prop.flatten_on_breach);
    if (p.contains("daily_reset_hour_utc") &&
        p["daily_reset_hour_utc"].is_number_integer()) {
      cfg.prop.daily_reset_hour_utc = p["daily_reset_hour_utc"].get<int>();
    }

    // a zero or >100% limit silently disables half the protection, and a warn
    // fraction at 1.0 means the only warning is the breach itself.
    if (cfg.prop.enabled) {
      // a prop-enabled config silently armed the survival guard against the
      // compiled-in 10000 when initial_balance was absent. every drawdown
      // percentage is a fraction of that base, so a $25k account with no
      // initial_balance was halted at 4% of 10000 = $400 down, not $1000.
      // the factory seeds PaperTrader from the same field, so the two agree --
      // they just both describe an account the config never named. refuse at
      // load, the same way prop.initial_balance is refused below, rather than
      // run against capital nobody chose.
      if (!has_initial_balance) {
        throw std::runtime_error(
            "config error: a config with a 'prop' block needs an explicit "
            "'initial_balance' at the root; drawdown limits are fractions of "
            "that base, and falling back to the compiled-in 10000 would arm "
            "the guard against an account the config never described");
      }
      auto bad_pct = [](double v) { return !(v > 0.0) || v > 1.0; };
      if (bad_pct(cfg.prop.max_daily_drawdown_pct)) {
        throw std::runtime_error(
            "config error: prop.max_daily_drawdown_pct must be a fraction in "
            "(0, 1], e.g. 0.04");
      }
      if (bad_pct(cfg.prop.max_total_drawdown_pct)) {
        throw std::runtime_error(
            "config error: prop.max_total_drawdown_pct must be a fraction in "
            "(0, 1], e.g. 0.06");
      }
      if (!(cfg.prop.warn_fraction > 0.0) || cfg.prop.warn_fraction >= 1.0) {
        throw std::runtime_error(
            "config error: prop.warn_fraction must be in (0, 1), e.g. 0.8");
      }
      if (cfg.prop.daily_reset_hour_utc < 0 ||
          cfg.prop.daily_reset_hour_utc > 23) {
        throw std::runtime_error(
            "config error: prop.daily_reset_hour_utc must be 0..23");
      }
    }
  }

  // --- startup data-integrity gate ---
  // sits next to prop rather than under a strategy block: like the account's
  // survival limits, this is an operational precondition, not a tuning knob.
  if (src.contains("startup_probe") && src["startup_probe"].is_object()) {
    const auto& p = src["startup_probe"];

    const std::string preset = SafeString(p, "preset", "");
    if (preset == "production") {
      cfg.probe = mokuren::integrity::ProbeCriteria::Production();
    } else if (preset == "development") {
      cfg.probe = mokuren::integrity::ProbeCriteria::Development();
    } else if (!preset.empty()) {
      throw std::runtime_error(
          "config error: startup_probe.preset must be 'development' or "
          "'production' (got '" + preset + "')");
    }

    // negative values are rejected rather than clamped: a typo like
    // "min_trades": -1000 would otherwise wrap through uint64_t and read as an
    // astronomically high minimum.
    auto read_count = [&p](const char* key, uint64_t fallback) -> uint64_t {
      if (!p.contains(key)) return fallback;
      const double v = SafeNumber(p, key, -1.0);
      if (!(v >= 0.0) || !std::isfinite(v)) {
        throw std::runtime_error(
            "config error: startup_probe." + std::string(key) +
            " must be a finite number >= 0");
      }
      return static_cast<uint64_t>(v);
    };
    auto read_ms = [&p](const char* key, int64_t fallback) -> int64_t {
      if (!p.contains(key)) return fallback;
      const double v = SafeNumber(p, key, -1.0);
      if (!(v >= 0.0) || !std::isfinite(v)) {
        throw std::runtime_error(
            "config error: startup_probe." + std::string(key) +
            " must be a finite number >= 0 (milliseconds)");
      }
      return static_cast<int64_t>(v);
    };

    // explicit keys win over the preset: soak for a minute, but lower the frame
    // counts for a thin instrument.
    cfg.probe.min_trades_per_min = read_count(
        "min_trades_per_min", cfg.probe.min_trades_per_min);
    cfg.probe.min_l2_frames_per_min = read_count(
        "min_l2_frames_per_min", cfg.probe.min_l2_frames_per_min);
    cfg.probe.min_duration_ms =
        read_ms("min_duration_ms", cfg.probe.min_duration_ms);
    cfg.probe.timeout_ms = read_ms("timeout_ms", cfg.probe.timeout_ms);
    cfg.probe.max_reconnects = static_cast<uint32_t>(
        read_count("max_reconnects", cfg.probe.max_reconnects));
    cfg.probe.min_l2_depth =
        read_count("min_l2_depth", cfg.probe.min_l2_depth);

    const std::string on_failure = SafeString(p, "on_failure", "");
    if (on_failure == "abort") {
      cfg.probe_abort_on_failure = true;
    } else if (on_failure == "warn") {
      cfg.probe_abort_on_failure = false;
    } else if (!on_failure.empty()) {
      throw std::runtime_error(
          "config error: startup_probe.on_failure must be 'abort' or 'warn' "
          "(got '" + on_failure + "')");
    }
  }

  // --- execution ---
  ParseSection("execution", [&]() {
    // fees and slippage change every pnl number in the report, so they are
    // never guessed. accepted at the root or, for older configs, inside
    // "environment".
    const json* ex = nullptr;
    std::string section;
    if (src.contains("execution") && src["execution"].is_object()) {
      ex = &src["execution"];
      section = "execution";
    } else if (src.contains("environment") && src["environment"].is_object()) {
      ex = &src["environment"];
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
    if (src.contains("triggers")) {
      arr = &src["triggers"];
    } else if (src.contains("strategy") && src["strategy"].contains("triggers")) {
      arr = &src["strategy"]["triggers"];
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
    if (src.contains("filters")) {
      arr = &src["filters"];
    } else if (src.contains("strategy") && src["strategy"].contains("filters")) {
      arr = &src["strategy"]["filters"];
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
    if (!src.contains("risk") || !src["risk"].is_object()) {
      throw std::runtime_error("config error: missing required section 'risk'");
    }
    {
      const auto& risk = src["risk"];
      // 1R is defined by sl_pct and nothing else.
      cfg.sl_pct = RequireNumberAny(risk, {"sl_pct", "sl_price_pct"}, "risk");
      // tp_r (a multiple of sl_pct, i.e. of 1R) wins over the legacy absolute
      // tp_pct. a config that names neither fails to load instead of keeping
      // a 1% guess.
      cfg.tp_pct = mokuren::ResolveTpPct(src, cfg.sl_pct);
      // remember the multiple the config wrote. ResolveTpPct honours tp_r only
      // when it is a json number, so mirror that exact rule here -- reading a
      // numeric string it ignored would let Validate report an R that never
      // produced the value under test.
      if (risk.contains("tp_r") && risk["tp_r"].is_number()) {
        cfg.tp_r = risk["tp_r"].get<double>();
      }
      cfg.risk_per_trade_pct = RequireNumber(risk, "risk_per_trade_pct", "risk");
      // optional by design: 0 (the default here) means "never widen the
      // strategy's own stop", so absence has a defined, non-arbitrary meaning.
      cfg.min_distance_dollars = SafeNumber(risk, "min_distance_dollars", 0.0);
      // risk.max_daily_loss is no longer read. a config that still carries it
      // loads unchanged -- the key is ignored, not an error -- because the
      // account's daily limit is prop.max_daily_drawdown_pct now: one anchor
      // (day-start equity), one unit (fraction of the funded balance) and one
      // reset hour, instead of a dollar amount measured from realised pnl.

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
    if (!src.contains("trade_management") || !src["trade_management"].is_object()) {
      throw std::runtime_error(
          "config error: missing required section 'trade_management'");
    }
    {
      const auto& tm = src["trade_management"];
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
  cfg.Validate(require_venue_credentials);

  return cfg;
}
