#include "config_parser.hpp"
#include "config_schema.hpp"
#include "trade_management.hpp"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <sstream>

// This file is at global scope (it fully qualifies mokuren::RiskPcts,
// mokuren::venue::ParseEnv, ... and only AppConfig/GridScanner are re-exported
// by config_parser.hpp). So `cfg::` alone has nothing to bind to -- the schema
// namespace is mokuren::cfg. One alias here beats spelling mokuren::cfg:: at
// every call site.
namespace cfg = mokuren::cfg;

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

void FlattenGridsRec(json& j) {
  if (j.is_object()) {
    // one "is this a sweep" predicate, the table's (config.md D5/9): shape
    // only, no Field, so component-array grids still flatten.
    if (cfg::IsGridNode(j)) {
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

// A component is disabled when any of the three spellings says so. They live
// inside component arrays (triggers, filters, risk.modules), which the schema
// deliberately does not model, so the three names travel with the data and are
// honoured here rather than in the table.
bool ComponentDisabled(const json& params) {
  for (const char* key : {"active", "enabled", "isActive"}) {
    if (!params.contains(key)) continue;
    const auto& v = params[key];
    if (v.is_boolean() && !v.get<bool>()) return true;
    if (v.is_string() && (v.get<std::string>() == "false" ||
                          v.get<std::string>() == "0")) {
      return true;
    }
    if (v.is_number() && v.get<double>() == 0.0) return true;
  }
  return false;
}

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
  // blofin signs with three credentials: key, secret and passphrase. a blank
  // passphrase surfaces as a signature failure at the first signed call, long
  // after startup, so it is checked here with the other two.
  if (require_venue_credentials && exchange == "blofin") {
    if (api_key.empty()) {
      fail("api_key must not be empty when exchange is \"blofin\"");
    }
    if (api_secret.empty()) {
      fail("api_secret must not be empty when exchange is \"blofin\"");
    }
    if (api_passphrase.empty()) {
      fail("api_passphrase must not be empty when exchange is \"blofin\"");
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
  if (exchange != "bybit" && exchange != "hyperliquid" &&
      exchange != "blofin") {
    fail("exchange must be one of 'bybit', 'hyperliquid', 'blofin' (got '" +
         exchange + "')");
  }
  // the feed's venue is whitelisted too: a typo there resolves to a bogus spec
  // in the streamer, which is an empty host at the first tick rather than a
  // startup error.
  if (!data_exchange.empty() && data_exchange != "bybit" &&
      data_exchange != "hyperliquid" && data_exchange != "blofin") {
    fail("venue.data_exchange must be one of 'bybit', 'hyperliquid', 'blofin' "
         "(got '" + data_exchange + "')");
  }
  // blofin has no testnet, and venue::Resolve throws on it. catching it here
  // names the config key instead of the factory call.
  if (DataExchange() == "blofin" &&
      DataEnv() == mokuren::venue::Env::Testnet) {
    fail("blofin has no testnet: set venue.data_environment to 'mainnet' or "
         "'demo'");
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

  // --- prop ---
  // bounds are checked only when the guard is armed: a disabled block must not
  // abort a run for a limit that will never be evaluated.
  if (prop.enabled) {
    auto bad_pct = [](double v) { return !(v > 0.0) || v > 1.0; };
    if (bad_pct(prop.max_daily_drawdown_pct)) {
      fail("prop.max_daily_drawdown_pct must be a fraction in (0, 1], e.g. "
           "0.04 (got " + std::to_string(prop.max_daily_drawdown_pct) + ")");
    }
    if (bad_pct(prop.max_total_drawdown_pct)) {
      fail("prop.max_total_drawdown_pct must be a fraction in (0, 1], e.g. "
           "0.06 (got " + std::to_string(prop.max_total_drawdown_pct) + ")");
    }
    if (!(prop.warn_fraction > 0.0) || prop.warn_fraction >= 1.0) {
      fail("prop.warn_fraction must be in (0, 1), e.g. 0.8 (got " +
           std::to_string(prop.warn_fraction) + ")");
    }
    if (prop.daily_reset_hour_utc < 0 || prop.daily_reset_hour_utc > 23) {
      fail("prop.daily_reset_hour_utc must be 0..23 (got " +
           std::to_string(prop.daily_reset_hour_utc) + ")");
    }
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
    if (!ComponentDisabled(t.params)) any_trigger_active = true;
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
  // the instrument is shared; the venue and the world may differ. DataExchange
  // and DataEnv both fall back to the account's values, so a config that names
  // neither resolves both specs to the same place.
  return mokuren::venue::Resolve(DataExchange(), DataEnv(), category, symbol);
}

mokuren::venue::Spec AppConfig::ExecSpec() const {
  return mokuren::venue::Resolve(exchange, env, category, symbol);
}

// ============================================================================
// AppConfig::Load
// ============================================================================





AppConfig AppConfig::Load(const json& j, bool require_venue_credentials) {
  // `conf`, not `cfg`: this function is inside namespace mokuren, so a local
  // named `cfg` shadows mokuren::cfg and every `cfg::GetX` below reads as
  // member access on this object ("'cfg' is not a class, namespace, or
  // enumeration"). The table keeps its documented spelling; only the builder
  // is renamed.
  AppConfig conf;

  // collapse any grid node ({"min":..,"max":..,"step":..,"val":..}) into its
  // scalar "val" before reading. the TUI seeds configs with grid nodes on
  // purpose so GridScanner can sweep them; the normal run path (single
  // backtest, --paper, --live) must see plain numbers. FlattenGrids is a no-op
  // on an already-flat config, so this is safe on every path.
  //
  // the backtest grid path (GridRunner/WfaRunner) calls GenerateGrid on the
  // ORIGINAL json first, then Load on each permutation's full_json -- which
  // Generate already wrote as scalars -- so flattening here does not disturb
  // the sweep.
  json flat = j;
  FlattenGrids(flat);
  const json& src = flat;

  // prop.initial_balance is a second owner of the account's base capital. the
  // table does not carry it, so RejectUnknownKeys would catch it too -- but
  // "unknown key" does not say WHY it is refused or where the number lives
  // now. This check has to run FIRST or the specific message is unreachable.
  if (src.contains("prop") && src["prop"].is_object() &&
      src["prop"].contains("initial_balance")) {
    throw std::runtime_error(
        "config error: prop.initial_balance is no longer read; total drawdown "
        "is measured against initial_balance (the account seed)");
  }

  // one table, one spelling per field (config.md D1/10.13). a key the schema
  // does not own is an error, not a warning: profiles are rebuilt against the
  // table, so a stale spelling must stop the run instead of half-applying.
  cfg::RejectUnknownKeys(src);

  // --- account ---
  // credentials live at the root of the config. grouping them under "exchange"
  // or "environment" is a removed spelling (config.md 10.13).
  conf.exchange = cfg::GetString(src, "exchange");
  // required, with no default: it is the denominator of every drawdown figure
  // and the capital PaperTrader is seeded with (config.md D7).
  conf.initial_balance = cfg::GetDouble(src, "initial_balance");
  conf.api_key = cfg::GetString(src, "api_key");
  conf.api_secret = cfg::GetString(src, "api_secret");
  // blofin's third credential. optional on every other venue, so absence is
  // not an error here -- Validate() enforces it only when exchange=="blofin".
  conf.api_passphrase = cfg::GetString(src, "api_passphrase");

  // --- credentials from the environment -----------------------------------
  // a config that names a live account is meant to be committed and shared; a
  // key is not. explicit values in the file win, and the variables are named
  // after the EXECUTION venue, so a config cannot pick up another venue's key
  // by accident: BLOFIN_API_KEY / BLOFIN_API_SECRET / BLOFIN_API_PASSPHRASE,
  // BYBIT_API_KEY / BYBIT_API_SECRET.
  {
    std::string prefix = conf.exchange;
    for (char& c : prefix) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    auto from_env = [&prefix](const char* suffix) -> std::string {
      const char* v = std::getenv((prefix + suffix).c_str());
      return v != nullptr ? std::string(v) : std::string();
    };
    if (conf.api_key.empty()) conf.api_key = from_env("_API_KEY");
    if (conf.api_secret.empty()) conf.api_secret = from_env("_API_SECRET");
    if (conf.api_passphrase.empty()) {
      conf.api_passphrase = from_env("_API_PASSPHRASE");
    }
  }

  // --- environment ---
  conf.filepath = cfg::GetString(src, "environment.dataset");
  conf.interval_ms = cfg::GetInt(src, "environment.interval_ms");
  conf.macro_interval_ms = cfg::GetInt(src, "environment.macro_interval_ms");
  conf.tick_size = cfg::GetDouble(src, "environment.tick_size");
  conf.vwap_reset_hour = cfg::GetInt(src, "environment.vwap_reset_hour");
  // 0 is the "auto" sentinel: the table cannot name a machine-dependent
  // number, so the loader resolves it (config.md 4). the coercion also fixes
  // "max_cores": 6.0, which is_number_integer() used to ignore (config.md 10.8).
  {
    const int cores = cfg::GetInt(src, "environment.max_cores");
    if (cores > 0) {
      conf.max_cores = static_cast<unsigned int>(cores);
    } else {
      conf.max_cores = std::thread::hardware_concurrency();
      if (conf.max_cores == 0) conf.max_cores = 4;
    }
  }


  // --- venue identity ---
  // environment + category + symbol + position mode form ONE identity. the
  // broker and the streamer are both built from it, and neither may derive a
  // host or a symbol of its own -- that is how a config ended up streaming
  // mainnet data into a testnet account.
  {
    const std::string env_name = cfg::GetString(src, "venue.environment");
    conf.env = mokuren::venue::ParseEnv(env_name);
    // derived, never a config key: the broker reads it (config.md 15.3).
    conf.is_testnet = (conf.env != mokuren::venue::Env::Mainnet);
    conf.category = cfg::GetString(src, "venue.category");
    conf.symbol = cfg::GetString(src, "venue.symbol");
    conf.position_mode = cfg::GetString(src, "venue.position_mode");
    conf.lot_step = cfg::GetDouble(src, "venue.lot_step");
    conf.allow_mainnet = cfg::GetBool(src, "venue.allow_mainnet");

    // --- market-data feed -------------------------------------------------
    // optional, and the only part of the venue identity that may differ from
    // the account: a key is bound to one venue in one world, the public book
    // is free. the instrument stays shared -- a feed for a different symbol
    // would size a contract the engine never priced.
    //
    // empty means "the feed and the account are the same venue in the same
    // world", so a config written before the split keeps its exact meaning.
    const std::string data_env_name =
        cfg::GetString(src, "venue.data_environment");
    if (!data_env_name.empty()) {
      conf.data_env = mokuren::venue::ParseEnv(data_env_name);
    }
    const std::string data_exchange_name =
        cfg::GetString(src, "venue.data_exchange");
    if (!data_exchange_name.empty()) {
      conf.data_exchange = data_exchange_name;
    }
  }

  // --- funded-account survival rules ---
  // parsed as their own section and deliberately not under a strategy block:
  // these limits belong to the account, not to a parameter set. a fraction
  // (0.04) is the unit here, matching every other pct in this file.
  //
  // presence of the block is the opt-in, but "enabled" must be explicit: a
  // profile whose author wrote the block to arm it must not load silently
  // disarmed (config.md 10.1a). the bounds are checked in Validate(), together
  // with every other numeric limit.
  if (cfg::Has(src, "prop")) {
    if (!cfg::Has(src, "prop.enabled")) {
      throw std::runtime_error(
          "config error: 'prop' present but 'prop.enabled' is missing; add it "
          "explicitly (true to arm, false to disable)");
    }
    conf.prop.enabled = cfg::GetBool(src, "prop.enabled");
    conf.prop.max_daily_drawdown_pct =
        cfg::GetDouble(src, "prop.max_daily_drawdown_pct");
    conf.prop.max_total_drawdown_pct =
        cfg::GetDouble(src, "prop.max_total_drawdown_pct");
    conf.prop.warn_fraction = cfg::GetDouble(src, "prop.warn_fraction");
    conf.prop.flatten_on_breach = cfg::GetBool(src, "prop.flatten_on_breach");
    conf.prop.daily_reset_hour_utc =
        cfg::GetInt(src, "prop.daily_reset_hour_utc");
  }

  // --- startup data-integrity gate ---
  // sits next to prop rather than under a strategy block: like the account's
  // survival limits, this is an operational precondition, not a tuning knob.
  //
  // `preset` is a cascade a flat table cannot express (config.md 15.6a): it
  // picks the base values, and every key the profile writes overrides them.
  {
    const std::string preset = cfg::GetString(src, "startup_probe.preset");
    conf.probe = (preset == "production")
                     ? mokuren::integrity::ProbeCriteria::Production()
                     : mokuren::integrity::ProbeCriteria::Development();

    // explicit keys win over the preset: soak for a minute, but lower the frame
    // counts for a thin instrument. a negative count is rejected rather than
    // wrapped through uint64_t into an astronomically high minimum.
    auto pick = [&src](const char* key, uint64_t base) -> uint64_t {
      const std::string path = std::string("startup_probe.") + key;
      if (!cfg::Has(src, path)) return base;
      const int v = cfg::GetInt(src, path);
      if (v < 0) {
        throw std::runtime_error("config error: '" + path +
                                 "' must be >= 0 (got " + std::to_string(v) +
                                 ")");
      }
      return static_cast<uint64_t>(v);
    };
    conf.probe.min_trades_per_min =
        pick("min_trades_per_min", conf.probe.min_trades_per_min);
    conf.probe.min_l2_frames_per_min =
        pick("min_l2_frames_per_min", conf.probe.min_l2_frames_per_min);
    conf.probe.min_duration_ms = static_cast<int64_t>(pick(
        "min_duration_ms", static_cast<uint64_t>(conf.probe.min_duration_ms)));
    conf.probe.timeout_ms = static_cast<int64_t>(
        pick("timeout_ms", static_cast<uint64_t>(conf.probe.timeout_ms)));
    conf.probe.max_reconnects = static_cast<uint32_t>(
        pick("max_reconnects", conf.probe.max_reconnects));
    conf.probe.min_l2_depth = pick("min_l2_depth", conf.probe.min_l2_depth);

    // absent leaves AppConfig's own default in place, as before.
    if (cfg::Has(src, "startup_probe.on_failure")) {
      conf.probe_abort_on_failure =
          cfg::GetString(src, "startup_probe.on_failure") == "abort";
    }
  }

  // --- execution ---
  // fees and slippage change every pnl number in the report, so they are never
  // guessed, and never read from "environment" (config.md 10.13).
  conf.taker_fee_pct = cfg::GetDouble(src, "execution.taker_fee_pct");
  conf.slippage_pct = cfg::GetDouble(src, "execution.slippage_pct");

  // --- components ---
  // triggers, filters and risk modules are arrays of {name, ...params}. the
  // table does not model them (config.md section 5): the loader reads the name
  // and hands the entry over verbatim, and the factory interprets the rest.
  auto read_components = [&src](const char* key,
                                std::vector<ComponentConfig>& out) {
    // `key` is a dotted path: "triggers" at the root, "risk.modules" one
    // level down. Lookup walks it -- src.contains("risk.modules") would be
    // false and silently load no modules.
    const json* arr = cfg::Lookup(src, key);
    if (arr == nullptr || !arr->is_array()) return;
    for (const auto& item : *arr) {
      if (!item.is_object()) continue;
      const std::string name = item.contains("name") && item["name"].is_string()
                                   ? item["name"].get<std::string>()
                                   : std::string();
      out.push_back({name, item});
    }
  };
  read_components("triggers", conf.triggers);
  read_components("filters", conf.filters);

  // --- risk ---
  // sl_pct is 1R and tp_r is the target in R. ResolveRiskPcts is the single
  // reader of that pair (config.md 15.7); everything downstream reads
  // AppConfig.sl_pct / tp_pct, so no second resolution can disagree.
  {
    const mokuren::RiskPcts rp = mokuren::ResolveRiskPcts(src);
    conf.sl_pct = rp.sl;
    conf.tp_pct = rp.tp;
    conf.tp_r = cfg::GetDouble(src, "risk.tp_r");
    conf.risk_per_trade_pct = cfg::GetDouble(src, "risk.risk_per_trade_pct");
    // 0 (the default) means "never widen the strategy's own stop".
    conf.min_distance_dollars = cfg::GetDouble(src, "risk.min_distance_dollars");
    // risk.max_daily_loss is a removed spelling: the account's daily limit is
    // prop.max_daily_drawdown_pct now (one anchor, one unit, one reset hour).
    // RejectUnknownKeys stops a profile that still carries it.

    read_components("risk.modules", conf.risk_modules);
  }

  // --- trade_management ---
  // four sub-blocks, four enablement policies. the table carries all four
  // flags and defaults (config.md 5.1), so the loader does not decide them;
  // the levels are in R and stay required whether or not a block is armed.
  if (!src.contains("trade_management") || !src["trade_management"].is_object()) {
    throw std::runtime_error(
        "config error: missing required section 'trade_management'");
  }
  {
    const auto& tm = src["trade_management"];
    if (!tm.contains("break_even") || !tm["break_even"].is_object()) {
      throw std::runtime_error(
          "config error: missing required section 'trade_management.break_even'");
    }
    const auto& be = tm["break_even"];
    conf.tm_config.be_trigger_pct = mokuren::ResolveLevelPct(
        be, conf.sl_pct, "trigger_r", "trigger_pct",
        "trade_management.break_even");
    conf.tm_config.be_target_pct = mokuren::ResolveLevelPct(
        be, conf.sl_pct, "target_r", "target_pct",
        "trade_management.break_even");
    conf.tm_config.enable_break_even =
        cfg::GetBool(src, "trade_management.break_even.enabled");

    if (!tm.contains("trailing") || !tm["trailing"].is_object()) {
      throw std::runtime_error(
          "config error: missing required section 'trade_management.trailing'");
    }
    const auto& tr = tm["trailing"];
    conf.tm_config.enable_trailing =
        cfg::GetBool(src, "trade_management.trailing.enabled");
    conf.tm_config.trailing_trigger_pct = mokuren::ResolveLevelPct(
        tr, conf.sl_pct, "trigger_r", "trigger_pct",
        "trade_management.trailing");
    conf.tm_config.trailing_dist_pct = mokuren::ResolveLevelPct(
        tr, conf.sl_pct, "distance_r", "distance_pct",
        "trade_management.trailing");

    if (!tm.contains("scale_out") || !tm["scale_out"].is_object()) {
      throw std::runtime_error(
          "config error: missing required section 'trade_management.scale_out'");
    }
    const auto& so = tm["scale_out"];
    conf.tm_config.enable_scale_out =
        cfg::GetBool(src, "trade_management.scale_out.enabled");
    conf.tm_config.scale_out_trigger_pct = mokuren::ResolveLevelPct(
        so, conf.sl_pct, "trigger_r", "trigger_pct",
        "trade_management.scale_out");
    conf.tm_config.scale_out_fraction =
        cfg::GetDouble(src, "trade_management.scale_out.fraction");

    // Optional structural-exit block. Absent = no-op. When enabled, the
    // resolver validates min_holdtime_ms and the rules list itself; kRequired
    // cannot express "required only when enabled" (config.md 15.5). Per-rule
    // params are stored verbatim and interpreted at build time in
    // structural_exit.hpp, mirroring the trigger pipeline.
    conf.tm_config.structural_exit = mokuren::ResolveStructuralExitConfig(tm);
  }

  // fail fast: abort before any EngineInstance is built
  conf.Validate(require_venue_credentials);

  return conf;
}
