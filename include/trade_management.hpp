#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

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

} // namespace mokuren
