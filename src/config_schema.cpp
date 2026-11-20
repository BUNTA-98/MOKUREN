#include "config_schema.hpp"

#include <cctype>
#include <climits>
#include <cmath>
#include <stdexcept>
#include <string>

namespace mokuren::cfg {
namespace {

// ---------------------------------------------------------------------------
// path lookup
// ---------------------------------------------------------------------------

const json* LookupImpl(const json& src, std::string_view path) {
  if (path.empty()) return nullptr;
  const json* cur = &src;
  std::size_t pos = 0;
  while (true) {
    const std::size_t dot = path.find('.', pos);
    const std::size_t len =
        (dot == std::string_view::npos) ? path.size() - pos : dot - pos;
    const std::string key(path.substr(pos, len));
    if (!cur->is_object()) return nullptr;
    auto it = cur->find(key);
    if (it == cur->end()) return nullptr;
    cur = &*it;
    if (dot == std::string_view::npos) break;
    pos = dot + 1;
  }
  return cur;
}

// ---------------------------------------------------------------------------
// coercion -- the spellings hand-written profiles actually use
// ---------------------------------------------------------------------------

bool CoerceDouble(const json& v, double& out) {
  if (v.is_number()) {
    const double d = v.get<double>();
    if (!std::isfinite(d)) return false;
    out = d;
    return true;
  }
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    std::size_t used = 0;
    try {
      const double d = std::stod(s, &used);
      while (used < s.size() &&
             std::isspace(static_cast<unsigned char>(s[used]))) {
        ++used;
      }
      if (used == s.size() && std::isfinite(d)) {
        out = d;
        return true;
      }
    } catch (...) {
    }
  }
  return false;
}

bool CoerceInt(const json& v, int& out) {
  if (v.is_number_integer()) {
    const long long x = v.get<long long>();
    if (x < INT_MIN || x > INT_MAX) return false;
    out = static_cast<int>(x);
    return true;
  }
  // whole-valued floats are accepted: "max_cores": 6.0 is a real profile
  // spelling, and the current loader's is_number_integer() silently drops it.
  if (v.is_number_float()) {
    const double d = v.get<double>();
    if (!std::isfinite(d) || std::floor(d) != d) return false;
    if (d < INT_MIN || d > INT_MAX) return false;
    out = static_cast<int>(d);
    return true;
  }
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    std::size_t used = 0;
    try {
      const long long x = std::stoll(s, &used);
      while (used < s.size() &&
             std::isspace(static_cast<unsigned char>(s[used]))) {
        ++used;
      }
      if (used != s.size()) return false;
      if (x < INT_MIN || x > INT_MAX) return false;
      out = static_cast<int>(x);
      return true;
    } catch (...) {
    }
  }
  return false;
}

bool CoerceBool(const json& v, bool& out) {
  if (v.is_boolean()) {
    out = v.get<bool>();
    return true;
  }
  if (v.is_number_integer()) {
    out = v.get<long long>() != 0;
    return true;
  }
  if (v.is_number_float()) {
    out = v.get<double>() != 0.0;
    return true;
  }
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    if (s == "true" || s == "True" || s == "TRUE" || s == "1") {
      out = true;
      return true;
    }
    if (s == "false" || s == "False" || s == "FALSE" || s == "0") {
      out = false;
      return true;
    }
  }
  return false;
}

bool EnumContains(const char* list, const std::string& value) {
  if (list == nullptr) return true;
  const std::string_view all(list);
  std::size_t pos = 0;
  while (pos <= all.size()) {
    const std::size_t bar = all.find('|', pos);
    const std::size_t len =
        (bar == std::string_view::npos) ? all.size() - pos : bar - pos;
    if (all.substr(pos, len) == value) return true;
    if (bar == std::string_view::npos) break;
    pos = bar + 1;
  }
  return false;
}

// ---------------------------------------------------------------------------
// resolution helpers
// ---------------------------------------------------------------------------

// The file's value for a field, with a sweepable grid node collapsed to its
// `val`. nullptr when absent.
//
// Two errors live here rather than in the getters: they are about the SHAPE of
// the value, and every kind has to reject them the same way (config.md 9). A
// {min,max,step} node with no scalar val would otherwise read as "not a
// number", and a grid on a field the table did not mark sweepable is a sweep
// the loader would silently ignore.
const json* ResolveValue(const json& src, const Field& f, json& scratch) {
  const json* v = LookupImpl(src, f.path);
  if (v == nullptr) return nullptr;
  if (v->is_object() && v->contains("min") && v->contains("max") &&
      v->contains("step") && (*v)["min"].is_number() &&
      (*v)["max"].is_number() && (*v)["step"].is_number()) {
    if (!IsGridNode(*v)) {
      throw std::runtime_error("config error: '" + std::string(f.path) +
                               "' is missing a scalar 'val' (a grid node needs "
                               "min, max, step and val)");
    }
    if ((f.flags & kSweepable) == 0) {
      throw std::runtime_error("config error: '" + std::string(f.path) +
                               "' cannot be swept");
    }
    scratch = (*v)["val"];
    return &scratch;
  }
  return v;
}

[[noreturn]] void MissingRequired(std::string_view path) {
  throw std::runtime_error("config error: missing required key '" +
                           std::string(path) + "'");
}

[[noreturn]] void BadType(std::string_view path, const char* want,
                          const json& got) {
  throw std::runtime_error("config error: '" + std::string(path) + "' must be " +
                           want + " (got " + got.dump() + ")");
}

[[noreturn]] void BadDefault(std::string_view path) {
  throw std::runtime_error(
      "config error: internal: bad table default for '" + std::string(path) +
      "' (kind and default_value disagree)");
}

} // namespace

// ---------------------------------------------------------------------------
// the table
// ---------------------------------------------------------------------------
//
// order doubles as sort key and category order: each category starts at a
// distinct base so "smallest order in the category" is deterministic.
//   100 Account      200 Environment   300 Venue     400 Execution
//   500 Risk         600 Trade Mgmt    700 Prop      800 Startup Probe
//
// Defaults are a consolidation of the current loader fallbacks and
// configs/_defaults.json, then made consistent. They are meant to be sensible,
// not to reproduce every historical number; see config.md 5.1.

const std::vector<Field>& Schema() {
  static const std::vector<Field> kSchema = {
      // --- Account -----------------------------------------------------------
      // enum_values is what the TUI cycles, and it is also why an unsupported
      // venue fails in GetString ("must be one of ...") instead of reaching
      // Validate()'s longer explanation about binance's depth-1 feed. That is
      // the right trade: a row that cannot cycle the venue names is worse than
      // a shorter error, and the rationale lives in Validate's comment.
      {"exchange", Kind::Enum, "bybit", "Account", "General", "exchange", 100,
       kRequired, 0.0, 0.0, "bybit|hyperliquid|blofin"},
      // promoted to required by D7: the denominator of every drawdown figure
      // and the capital PaperTrader is seeded with.
      {"initial_balance", Kind::Double, 25000.0, "Account", "General",
       "initial_balance", 110, kRequired},
      {"api_key", Kind::String, "", "Account", "Credentials", "api_key", 120},
      {"api_secret", Kind::String, "", "Account", "Credentials", "api_secret",
       130},
      {"api_passphrase", Kind::String, "", "Account", "Credentials",
       "api_passphrase", 140},

      // --- Environment -------------------------------------------------------
      // "data/" is a real default now that DataManager::ResolveDatasetRoot
      // maps a bare root to the newest dataset beneath it (config.md 15.9).
      // Before that change it resolved to an empty channel, which the loader
      // calls valid -- a silent no-op run.
      {"environment.dataset", Kind::DirPath, "data/", "Environment", "General",
       "dataset", 200, 0, 0.0, 0.0, nullptr, "data"},
      {"environment.interval_ms", Kind::Int, 60000, "Environment", "General",
       "interval_ms", 210, kRequired, 1.0, 3600000.0},
      {"environment.macro_interval_ms", Kind::Int, 900000, "Environment",
       "General", "macro_interval_ms", 220, kRequired, 1.0, 86400000.0},
      {"environment.tick_size", Kind::Double, 0.1, "Environment", "General",
       "tick_size", 230, kRequired, 0.0, 1.0},
      {"environment.vwap_reset_hour", Kind::Int, 0, "Environment", "General",
       "vwap_reset_hour", 240, kRequired, 0.0, 23.0},
      // 0 means auto (hardware_concurrency); a sentinel the loader resolves,
      // not a default_fn (config.md 4).
      {"environment.max_cores", Kind::Int, 0, "Environment", "General",
       "max_cores", 250, 0, 0.0, 256.0},

      // --- Venue -------------------------------------------------------------
      {"venue.environment", Kind::Enum, "testnet", "Venue", "General",
       "environment", 300, 0, 0.0, 0.0, "mainnet|testnet|demo"},
      {"venue.category", Kind::Enum, "linear", "Venue", "General", "category",
       310, 0, 0.0, 0.0, "linear|spot|inverse"},
      {"venue.symbol", Kind::String, "BTCUSDT", "Venue", "General", "symbol",
       320},
      {"venue.position_mode", Kind::Enum, "one_way", "Venue", "General",
       "position_mode", 330, 0, 0.0, 0.0, "one_way|hedge"},
      {"venue.lot_step", Kind::Double, 0.001, "Venue", "General", "lot_step",
       340, 0, 0.0, 1.0},
      {"venue.allow_mainnet", Kind::Bool, false, "Venue", "General",
       "allow_mainnet", 350},
      // empty == "same as the account" (config.md 10.13). Not an alias: the
      // sentinel is a value, the key name is still the only accepted spelling.
      {"venue.data_environment", Kind::Enum, "", "Venue", "Market data",
       "data_environment", 360, 0, 0.0, 0.0, "mainnet|testnet|demo"},
      {"venue.data_exchange", Kind::Enum, "", "Venue", "Market data",
       "data_exchange", 370, 0, 0.0, 0.0, "bybit|hyperliquid|blofin"},

      // --- Execution ---------------------------------------------------------
      {"execution.taker_fee_pct", Kind::Double, 0.0004, "Execution", "General",
       "taker_fee_pct", 400, kRequired, 0.0, 0.01},
      {"execution.slippage_pct", Kind::Double, 0.0002, "Execution", "General",
       "slippage_pct", 410, kRequired, 0.0, 0.01},

      // --- Risk --------------------------------------------------------------
      // sl_pct is 1R. Sweepable: it is the most-tuned number in the engine.
      {"risk.sl_pct", Kind::Double, 0.003, "Risk", "General", "sl_pct", 500,
       kRequired | kSweepable, 0.0001, 0.05},
      {"risk.tp_r", Kind::Double, 2.0, "Risk", "General", "tp_r", 510,
       kRequired | kSweepable, 0.1, 20.0},
      {"risk.risk_per_trade_pct", Kind::Double, 0.005, "Risk", "General",
       "risk_per_trade_pct", 520, kRequired, 0.0001, 0.05},
      {"risk.min_distance_dollars", Kind::Double, 0.0, "Risk", "General",
       "min_distance_dollars", 530, 0, 0.0, 1000.0},

      // --- Trade management --------------------------------------------------
      // Four sub-blocks, four enablement policies -- the per-field flag is the
      // only thing that can express that (config.md 5.1).
      {"trade_management.break_even.enabled", Kind::Bool, true,
       "Trade Management", "Break-even", "enabled", 600},
      {"trade_management.break_even.trigger_r", Kind::Double, 1.0,
       "Trade Management", "Break-even", "trigger_r", 610, kRequired, 0.0,
       20.0},
      {"trade_management.break_even.target_r", Kind::Double, 0.5,
       "Trade Management", "Break-even", "target_r", 620, kRequired, 0.0, 20.0},

      {"trade_management.trailing.enabled", Kind::Bool, false,
       "Trade Management", "Trailing", "enabled", 630, kRequired},
      {"trade_management.trailing.trigger_r", Kind::Double, 1.0,
       "Trade Management", "Trailing", "trigger_r", 640, kRequired, 0.0, 20.0},
      {"trade_management.trailing.distance_r", Kind::Double, 0.5,
       "Trade Management", "Trailing", "distance_r", 650, kRequired, 0.0, 20.0},

      {"trade_management.scale_out.enabled", Kind::Bool, false,
       "Trade Management", "Scale-out", "enabled", 660, kRequired},
      {"trade_management.scale_out.trigger_r", Kind::Double, 2.0,
       "Trade Management", "Scale-out", "trigger_r", 670, kRequired, 0.0, 20.0},
      {"trade_management.scale_out.fraction", Kind::Double, 0.5,
       "Trade Management", "Scale-out", "fraction", 680, kRequired, 0.1, 0.9},

      // Optional, defaults OFF -- the opposite polarity from prop, and safe:
      // an absent block and an enabled:false block are the same no-op. Its
      // min_holdtime_ms is required only when enabled (resolver-enforced, not
      // expressible as kRequired).
      {"trade_management.structural_exit.enabled", Kind::Bool, false,
       "Trade Management", "Structural exit", "enabled", 690},
      {"trade_management.structural_exit.min_holdtime_ms", Kind::Int, 180000,
       "Trade Management", "Structural exit", "min_holdtime_ms", 700, 0, 0.0,
       3600000.0},

      // --- Prop --------------------------------------------------------------
      // Optional and defaults OFF, but a prop block WITHOUT an explicit
      // "enabled" is a load error (config.md 10.1a) -- falling back to false
      // would silently disarm a block whose author wrote it to arm.
      {"prop.enabled", Kind::Bool, false, "Prop", "General", "enabled", 700},
      {"prop.max_daily_drawdown_pct", Kind::Double, 0.04, "Prop", "General",
       "max_daily_drawdown_pct", 710, 0, 0.0, 1.0},
      {"prop.max_total_drawdown_pct", Kind::Double, 0.06, "Prop", "General",
       "max_total_drawdown_pct", 720, 0, 0.0, 1.0},
      {"prop.warn_fraction", Kind::Double, 0.8, "Prop", "General",
       "warn_fraction", 730, 0, 0.0, 1.0},
      {"prop.flatten_on_breach", Kind::Bool, true, "Prop", "General",
       "flatten_on_breach", 740},
      {"prop.daily_reset_hour_utc", Kind::Int, 0, "Prop", "General",
       "daily_reset_hour_utc", 750, 0, 0.0, 23.0},

      // --- Startup probe -----------------------------------------------------
      // Transcribed from ProbeCriteria::Development(). `preset: production`
      // replaces these with Production()'s values for every key the profile
      // does not write -- a cascade a flat table cannot express, so `Load`
      // special-cases it (config.md 15.6a). These defaults are what an absent
      // block gets, which is why they must be Development()'s numbers.
      {"startup_probe.preset", Kind::Enum, "development", "Startup Probe",
       "General", "preset", 800, 0, 0.0, 0.0, "development|production"},
      {"startup_probe.min_trades_per_min", Kind::Int, 60, "Startup Probe",
       "General", "min_trades_per_min", 810, 0, 0.0, 100000.0},
      {"startup_probe.min_l2_frames_per_min", Kind::Int, 120, "Startup Probe",
       "General", "min_l2_frames_per_min", 820, 0, 0.0, 100000.0},
      {"startup_probe.min_duration_ms", Kind::Int, 5000, "Startup Probe",
       "General", "min_duration_ms", 830, 0, 0.0, 3600000.0},
      {"startup_probe.timeout_ms", Kind::Int, 30000, "Startup Probe",
       "General", "timeout_ms", 840, 0, 0.0, 3600000.0},
      {"startup_probe.max_reconnects", Kind::Int, 0, "Startup Probe", "General",
       "max_reconnects", 850, 0, 0.0, 1000.0},
      {"startup_probe.min_l2_depth", Kind::Int, 2, "Startup Probe", "General",
       "min_l2_depth", 860, 0, 0.0, 200.0},
      {"startup_probe.on_failure", Kind::Enum, "abort", "Startup Probe",
       "General", "on_failure", 870, 0, 0.0, 0.0, "abort|warn"},
  };
  return kSchema;
}

const Field* Find(std::string_view path) {
  for (const Field& f : Schema()) {
    if (path == f.path) return &f;
  }
  return nullptr;
}

const json* Lookup(const json& src, std::string_view path) {
  return LookupImpl(src, path);
}

bool Has(const json& src, std::string_view path) {
  return LookupImpl(src, path) != nullptr;
}

// ---------------------------------------------------------------------------
// getters
// ---------------------------------------------------------------------------

double GetDouble(const json& src, std::string_view path) {
  const Field* f = Find(path);
  if (f == nullptr) throw std::runtime_error(
      "config error: unknown key '" + std::string(path) + "'");
  json scratch;
  const json* v = ResolveValue(src, *f, scratch);
  if (v == nullptr) {
    if ((f->flags & kRequired) != 0) MissingRequired(path);
    double d = 0.0;
    if (!CoerceDouble(f->default_value, d)) BadDefault(path);
    return d;
  }
  double d = 0.0;
  if (!CoerceDouble(*v, d)) BadType(path, "a number", *v);
  return d;
}

int GetInt(const json& src, std::string_view path) {
  const Field* f = Find(path);
  if (f == nullptr) throw std::runtime_error(
      "config error: unknown key '" + std::string(path) + "'");
  json scratch;
  const json* v = ResolveValue(src, *f, scratch);
  if (v == nullptr) {
    if ((f->flags & kRequired) != 0) MissingRequired(path);
    int n = 0;
    if (!CoerceInt(f->default_value, n)) BadDefault(path);
    return n;
  }
  int n = 0;
  if (!CoerceInt(*v, n)) BadType(path, "an integer", *v);
  return n;
}

bool GetBool(const json& src, std::string_view path) {
  const Field* f = Find(path);
  if (f == nullptr) throw std::runtime_error(
      "config error: unknown key '" + std::string(path) + "'");
  json scratch;
  const json* v = ResolveValue(src, *f, scratch);
  if (v == nullptr) {
    if ((f->flags & kRequired) != 0) MissingRequired(path);
    bool b = false;
    if (!CoerceBool(f->default_value, b)) BadDefault(path);
    return b;
  }
  bool b = false;
  if (!CoerceBool(*v, b)) BadType(path, "a boolean", *v);
  return b;
}

std::string GetString(const json& src, std::string_view path) {
  const Field* f = Find(path);
  if (f == nullptr) throw std::runtime_error(
      "config error: unknown key '" + std::string(path) + "'");
  json scratch;
  const json* v = ResolveValue(src, *f, scratch);
  if (v == nullptr) {
    if ((f->flags & kRequired) != 0) MissingRequired(path);
    if (!f->default_value.is_string()) BadDefault(path);
    return f->default_value.get<std::string>();
  }
  if (!v->is_string()) BadType(path, "a string", *v);
  const std::string s = v->get<std::string>();
  if (f->enum_values != nullptr && !s.empty() && !EnumContains(f->enum_values, s)) {
    throw std::runtime_error("config error: '" + std::string(path) +
                             "' must be one of " + f->enum_values + " (got '" +
                             s + "')");
  }
  return s;
}

// ---------------------------------------------------------------------------
// grid nodes
// ---------------------------------------------------------------------------

bool IsGridNode(const json& j) {
  if (!j.is_object()) return false;
  for (const char* k : {"min", "max", "step"}) {
    if (!j.contains(k) || !j[k].is_number()) return false;
  }
  if (!j.contains("val")) return false;
  double d = 0.0;
  return CoerceDouble(j["val"], d);  // number or numeric string
}

bool IsSweepableValue(const json& j, const Field& f) {
  if ((f.flags & kSweepable) == 0) return false;
  return IsGridNode(j);
}

// ---------------------------------------------------------------------------
// unknown keys
// ---------------------------------------------------------------------------

namespace {

// Subtrees the table deliberately does not model (config.md 8.1). Their
// contents are component arrays or file-driven settings, interpreted by name
// further down the stack; the loader sees only the enclosing key.
bool IsAllowedSubtree(std::string_view path) {
  static const std::string_view kAllowed[] = {
      "triggers",
      "filters",
      "risk.modules",
      "trade_management.structural_exit.rules",
      "backtest",
  };
  for (std::string_view p : kAllowed) {
    if (path == p) return true;
  }
  return false;
}

// Collects rather than throws, so the TUI footer can show a count. One walk,
// two consumers: RejectUnknownKeys reports the first path it finds, which is
// the same path a throwing walk would have hit first.
void CollectUnknownRec(const json& node, const std::string& prefix,
                       std::vector<std::string>& out) {
  for (auto it = node.begin(); it != node.end(); ++it) {
    const std::string path =
        prefix.empty() ? it.key() : prefix + "." + it.key();
    if (IsAllowedSubtree(path)) continue;
    // A known field is the getters' business: they own the type error, and it
    // names the value the author actually wrote.
    if (Find(path) != nullptr) continue;
    if (it.value().is_object()) {
      CollectUnknownRec(it.value(), path, out);
      continue;
    }
    out.push_back(path);
  }
}

} // namespace

void RejectUnknownKeys(const json& src) {
  if (!src.is_object()) {
    throw std::runtime_error("config error: the config root must be an object");
  }
  std::vector<std::string> unknown;
  CollectUnknownRec(src, "", unknown);
  if (!unknown.empty()) {
    throw std::runtime_error("config error: unknown key '" + unknown.front() +
                             "'");
  }
}

std::size_t CountUnknownKeys(const json& src) {
  if (!src.is_object()) return 0;
  std::vector<std::string> unknown;
  CollectUnknownRec(src, "", unknown);
  return unknown.size();
}

} // namespace mokuren::cfg
