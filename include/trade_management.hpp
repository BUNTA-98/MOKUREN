#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace mokuren {

// Risk distances as fractions of price (0.005 == 0.5%).
struct RiskPcts {
  double sl = 0.004;
  double tp = 0.012;
};

// Single resolution point for the risk block. R-multiples everywhere: sl_pct
// is 1R, tp_r is the target in R, and every trade-management trigger is
// already expressed in R. A config that only carries an absolute tp_pct is
// converted to R here, once, so no downstream code ever sees two dialects.
inline RiskPcts ResolveRiskPcts(const nlohmann::json& root) {
  if (!root.contains("risk") || !root["risk"].is_object()) {
    throw std::runtime_error("config error: missing required section 'risk'");
  }
  const auto& risk = root["risk"];

  RiskPcts out;
  // sl_pct is 1R and is canonical; sl_price_pct is the legacy spelling and is
  // consulted only when sl_pct is absent. this used to check the legacy key
  // first, while AppConfig::Load reads the same pair with sl_pct first -- so a
  // config carrying both resolved to one 1R in the loader and a different one
  // in every path that overwrites cfg.sl_pct from here, and the placed-stop
  // ladder was measured against whichever won.
  //
  // numeric strings are accepted because RequireNumberAny accepts them: this
  // runs after AppConfig::Load has already approved the config, and rejecting a
  // spelling the loader took turned a valid run into a "key is required" abort.
  auto read_sl = [&risk](const char* key, double& out) {
    if (!risk.contains(key)) return false;
    const auto& v = risk[key];
    if (v.is_number()) {
      out = v.get<double>();
      return true;
    }
    if (v.is_string()) {
      const std::string s = v.get<std::string>();
      size_t used = 0;
      try {
        const double parsed = std::stod(s, &used);
        if (used == s.size()) {
          out = parsed;
          return true;
        }
      } catch (...) {
        // an unparseable string is a missing key here
      }
    }
    return false;
  };
  if (!read_sl("sl_pct", out.sl) && !read_sl("sl_price_pct", out.sl)) {
    throw std::runtime_error(
        "config error: 'risk.sl_pct' (or legacy 'risk.sl_price_pct') is "
        "required and defines 1R");
  }
  // sl is the divisor that turns an absolute tp_pct into R. Load's bounds
  // check runs after this on every real path, but a direct caller (tests,
  // tooling) reached the division below with sl == 0 and got NaN, not an
  // error.
  if (!(out.sl > 0.0)) {
    throw std::runtime_error(
        "config error: 'risk.sl_pct' must be > 0; it defines 1R");
  }

  double tp_r = 0.0;
  if (risk.contains("tp_r") && risk["tp_r"].is_number()) {
    tp_r = risk["tp_r"].get<double>();
  } else if (risk.contains("tp_pct") && risk["tp_pct"].is_number()) {
    // legacy key: an absolute take-profit percent. normalise to R on read.
    tp_r = risk["tp_pct"].get<double>() / out.sl;
  } else {
    throw std::runtime_error(
        "config error: 'risk.tp_r' (or legacy 'risk.tp_pct') is required");
  }
  out.tp = out.sl * tp_r;
  return out;
}

// Live/paper take-profit as a percent of entry. tp_r is canonical (a multiple
// of sl_pct, i.e. of 1R); tp_pct is the legacy absolute key and is consulted
// only when tp_r is absent. A config that names neither is rejected rather
// than silently inheriting a default.
inline double ResolveTpPct(const nlohmann::json& root, double sl_pct) {
  if (!root.contains("risk") || !root["risk"].is_object()) {
    throw std::runtime_error("config error: missing required section 'risk'");
  }
  const auto& risk = root["risk"];
  if (risk.contains("tp_r") && risk["tp_r"].is_number()) {
    return risk["tp_r"].get<double>() * sl_pct;
  }
  if (risk.contains("tp_pct") && risk["tp_pct"].is_number()) {
    return risk["tp_pct"].get<double>();
  }
  throw std::runtime_error(
      "config error: 'risk.tp_r' (or legacy 'risk.tp_pct') is required");
}

// One trade-management level as a percent of entry, the unit PaperTrader and
// the venue brokers already compare against. The R key is canonical; the
// legacy absolute key is read only when the R key is absent. Because sl_pct
// is 1R, the same config now means the same thing in scan, WFA, replay and
// live -- previously the scanner read trigger_r while live read trigger_pct
// from the same file and quietly used its default instead. a block that names
// neither key is rejected.
inline double ResolveLevelPct(const nlohmann::json& block, double sl_pct,
                              const char* r_key, const char* pct_key,
                              const char* section) {
  if (block.contains(r_key) && block[r_key].is_number()) {
    return block[r_key].get<double>() * sl_pct;
  }
  if (block.contains(pct_key) && block[pct_key].is_number()) {
    return block[pct_key].get<double>();
  }
  throw std::runtime_error(std::string("config error: '") + section +
                           "' requires '" + r_key + "' (or legacy '" +
                           pct_key + "'); nothing is defaulted");
}

// --- structural exits -----------------------------------------------------
//
// A structural exit is a THIRD, parallel close path on an open position:
// alongside (a) the broker's SL/TP and (b) the PositionManager's BE/trail/
// scale. Market structure / trade age / MFE drive the decision; whoever
// closes first wins. The guard never overrides a fixed stop -- the stop is the
// floor and a structural close is an early out, not a replacement.
//
// This struct is the DATA an ExitPlan is built from. Per-rule param types are
// NOT interpreted here; they are interpreted at build time in
// structural_exit.hpp. The split mirrors triggers: Load parses opaque JSON,
// the factory interprets names. A disabled rule cannot produce a bad signal,
// so a typo inside it cannot become a hidden failure.
struct ExitRuleSpec {
  std::string    name;
  bool           enabled = true;
  nlohmann::json params;
};

struct StructuralExitConfig {
  bool                      enabled = false;
  int64_t                   min_holdtime_ms = 0;
  std::vector<ExitRuleSpec> rules;
};

// Resolve the optional `structural_exit` block. Absent or enabled=false is a
// no-op: existing configs keep their exact meaning. When enabled,
// min_holdtime_ms is REQUIRED (>0) -- without it the guard can close the entry
// candle itself -- and at least one non-disabled rule is required (an
// all-disabled plan can never close a position, mirroring
// AppConfig::Validate's "every trigger is disabled" error). Per-rule params
// are stored verbatim; BuildExitPlan interprets them.
inline StructuralExitConfig ResolveStructuralExitConfig(const nlohmann::json& tm) {
  StructuralExitConfig out;
  if (!tm.is_object() || !tm.contains("structural_exit") ||
      !tm["structural_exit"].is_object()) {
    return out;  // feature off; nothing else validated
  }
  const auto& se = tm["structural_exit"];

  // Same enabled/active/isActive spellings the trigger, filter and
  // risk-module readers accept. Master switch default = false (plan
  // requirement: absent block or enabled=false must not change existing
  // configs).
  auto read_enabled = [](const nlohmann::json& block, const char* key,
                         bool fallback) {
    if (!block.is_object() || !block.contains(key)) return fallback;
    const auto& v = block[key];
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
  };

  out.enabled = read_enabled(se, "enabled", false);
  if (!out.enabled) {
    return out;  // feature off; rules are not parsed or validated
  }

  // --- min_holdtime_ms: REQUIRED, must be > 0 ------------------------------
  // No default: a config that enables structural exits must say how long a
  // position has to live before a rule can close it. Without this the guard
  // can close the entry candle itself -- a structural close on the same tick
  // the trade printed, with no time for anything to have developed.
  if (!se.contains("min_holdtime_ms")) {
    throw std::runtime_error(
        "config error: 'trade_management.structural_exit.min_holdtime_ms' is "
        "required when structural_exit.enabled is true (this is the minimum "
        "time a position must live before any rule can close it; there is no "
        "safe default)");
  }
  int64_t min_hold = 0;
  {
    const auto& mh = se["min_holdtime_ms"];
    if (mh.is_number_integer()) {
      min_hold = mh.get<int64_t>();
    } else if (mh.is_number_float()) {
      min_hold = static_cast<int64_t>(mh.get<double>());
    } else if (mh.is_string()) {
      const std::string s = mh.get<std::string>();
      size_t used = 0;
      try {
        min_hold = static_cast<int64_t>(std::stoll(s, &used));
        if (used != s.size()) {
          throw std::runtime_error(
              "config error: "
              "'trade_management.structural_exit.min_holdtime_ms' is not a "
              "whole number (got '" + s + "')");
        }
      } catch (const std::runtime_error&) {
        throw;  // re-throw our own message untouched
      } catch (...) {
        throw std::runtime_error(
            "config error: "
            "'trade_management.structural_exit.min_holdtime_ms' must be an "
            "integer number of milliseconds (got '" + s + "')");
      }
    } else {
      throw std::runtime_error(
          "config error: "
          "'trade_management.structural_exit.min_holdtime_ms' must be an "
          "integer number of milliseconds (got " + mh.dump() + ")");
    }
  }
  if (min_hold <= 0) {
    throw std::runtime_error(
        "config error: "
        "'trade_management.structural_exit.min_holdtime_ms' must be > 0 "
        "(got " + std::to_string(min_hold) + "); a zero value allows a "
        "structural close on the entry candle");
  }
  out.min_holdtime_ms = min_hold;

  // --- rules[]: same { name, enabled, params } shape as triggers -----------
  if (se.contains("rules") && se["rules"].is_array()) {
    for (const auto& item : se["rules"]) {
      if (!item.is_object()) continue;
      ExitRuleSpec spec;
      if (item.contains("name") && item["name"].is_string()) {
        spec.name = item["name"].get<std::string>();
      }
      // Per-rule disable uses the same three spellings IsDisabled honours in
      // the factory. Absent => enabled, matching the trigger pipeline.
      const bool explicitly_disabled =
          !read_enabled(item, "enabled", true) ||
          !read_enabled(item, "active", true) ||
          !read_enabled(item, "isActive", true);
      spec.enabled = !explicitly_disabled;
      spec.params = item;
      out.rules.push_back(std::move(spec));
    }
  }

  bool any_enabled = false;
  for (const auto& r : out.rules) {
    if (r.enabled && !r.name.empty()) {
      any_enabled = true;
      break;
    }
  }
  if (!any_enabled) {
    throw std::runtime_error(
        "config error: 'trade_management.structural_exit.enabled' is true but "
        "every rule is disabled (or the list is empty) -- the guard could "
        "never close a position; enable at least one rule or set the master "
        "switch to false");
  }

  return out;
}

} // namespace mokuren
