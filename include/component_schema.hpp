#pragma once
#include "config_schema.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace mokuren::cfg {

// The complete shape of a configurable profile, in one place (config.md 8.1).
//
// The UI used to derive the menu from the file: a component category appeared
// only if the json already carried that array, so a fresh profile showed no
// Filters, no Risk Modules and no exit rules -- and the only trigger ever
// offered was a name typed into the UI. The menu is now a function of WHAT THE
// ENGINE CAN BUILD (Catalogue) and WHAT THE TABLE DEFAULTS TO (Schema), never
// of the file. The file only supplies deviations.

struct ComponentDef {
  const char* array_path;  // dotted, e.g. "risk.modules"
  const char* name;        // the item's `name`
  nlohmann::json params;   // param key -> default, matching the factory
};

// Every component the factory can build, with the factory's own fallbacks
// (src/factory.cpp: MakeTrigger, the filter chain, the risk-module chain).
//
// Keep these numbers equal to the factory's. A mismatch only changes what the
// menu SHOWS before you edit -- never what a run reads, because the factory has
// its own fallback -- but a wrong default here is worse than none: it looks
// like the engine's opinion.
//
// The cheap cross-check is that every name below appears as a string literal in
// factory.cpp. A name the factory does not know is a load error at Build()
// ("unknown trigger ..."), so a typo here would arm a row that cannot run.
inline const std::vector<ComponentDef>& Catalogue() {
  static const std::vector<ComponentDef> kCatalogue = {
      // --- triggers (factory: MakeTrigger) ----------------------------------
      {"triggers", "DeltaAbsorption",
       nlohmann::json::object({{"min_delta_threshold", 1.0}})},
      {"triggers", "StackedImbalance",
       nlohmann::json::object({{"ratio_threshold", 1.5},
                               {"min_stacked_count", 2},
                               {"min_level_qty", 0.0001},
                               {"require_candle_agreement", false}})},
      {"triggers", "SpoofHunter",
       nlohmann::json::object({{"lookback_ms", 500},
                               {"min_wall_qty", 30.0},
                               {"drop_threshold", 0.9}})},
      // LiveImbalanceTrigger (include/trigger.hpp). The tick-side reporter: its
      // EvaluateCandle returns an empty signal on purpose and all the work is
      // in EvaluateTick, which latches one formation per bar. It is a separate
      // entry from StackedImbalance because they answer different questions --
      // that one reports at close, this one while the stack is forming.
      //
      // The param keys mirror StackedImbalance's because both scan through
      // imbalance::ScanFootprint; verify against the constructor if the header
      // drifted, since a key the constructor does not read is a row that tunes
      // nothing.
      {"triggers", "LiveImbalance",
       nlohmann::json::object({{"ratio_threshold", 1.5},
                               {"min_stacked_count", 2},
                               {"min_level_qty", 0.0001}})},

      // --- filters (factory: the `f.name == ...` chain) ---------------------
      {"filters", "MinVolume", nlohmann::json::object({{"min_volume", 1.0}})},
      {"filters", "VwapTrend",
       nlohmann::json::object({{"require_trend_alignment", true}})},
      {"filters", "CVDDivergence", nlohmann::json::object({{"lookback", 5}})},
      {"filters", "MacroTrend", nlohmann::json::object({{"lookback", 1440}})},
      {"filters", "Volatility",
       nlohmann::json::object({{"min_dollar", 150.0}, {"lookback", 5}})},
      {"filters", "TimeOfDay",
       nlohmann::json::object({{"start_h", 8},
                               {"start_m", 0},
                               {"end_h", 17},
                               {"end_m", 0}})},
      // no parameters: the filter reads the bar it is handed.
      {"filters", "POCTrend", nlohmann::json::object()},
      {"filters", "RVOL",
       nlohmann::json::object({{"threshold", 1.5}, {"lookback", 20}})},
      // the factory reads "lookback" here (not "period"), and passes it as the
      // ATR period -- the key name is what a config may write, so it is the one
      // that goes in the menu.
      {"filters", "ATRChop",
       nlohmann::json::object({{"min_atr", 20.0}, {"lookback", 14}})},
      {"filters", "OrderbookImbalance",
       nlohmann::json::object({{"ratio", 3.0}})},

      // --- risk modules (factory: the `r.name == ...` chain) ----------------
      {"risk.modules", "SinglePositionLock", nlohmann::json::object()},
      {"risk.modules", "MaxStopDistanceLock",
       nlohmann::json::object({{"max_stop_pct", 0.015}})},
      {"risk.modules", "MaxLeverageLock",
       nlohmann::json::object({{"max_leverage", 10.0}})},
      {"risk.modules", "AntiRevengeLock",
       nlohmann::json::object({{"cooldown_ms", 1800000}})},

      // --- structural exit rules (structural_exit.hpp: BuildExitPlan) -------
      // Names and param keys are the ones BuildExitPlan matches on, so a row
      // here is a rule the factory can actually construct. The master switch
      // (trade_management.structural_exit.enabled) stays off, so a profile that
      // arms nothing never reaches these.
      {"trade_management.structural_exit.rules", "MfeGiveback",
       nlohmann::json::object({{"giveback_r", 1.0}})},
      {"trade_management.structural_exit.rules", "TimeStop",
       nlohmann::json::object({{"max_hold_ms", 3600000}})},
      {"trade_management.structural_exit.rules", "ImbalanceBreak",
       nlohmann::json::object({{"ratio", 2.0}, {"min_levels", 3}})},
      {"trade_management.structural_exit.rules", "DeltaFlip",
       nlohmann::json::object({{"min_delta", 1.0}})},
      {"trade_management.structural_exit.rules", "SpoofPull",
       nlohmann::json::object({{"drop_pct", 0.9}, {"min_qty", 30.0}})},
      {"trade_management.structural_exit.rules", "DeltaDivergence",
       nlohmann::json::object({{"lookback", 5}})},
  };
  return kCatalogue;
}

// dotted -> json pointer. One owner for the mapping, shared with the UI.
inline nlohmann::json::json_pointer PointerFor(const std::string& dotted) {
  std::string p(dotted);
  std::replace(p.begin(), p.end(), '.', '/');
  return nlohmann::json::json_pointer("/" + p);
}

inline bool IsComponentPath(const std::string& dotted) {
  for (const ComponentDef& d : Catalogue()) {
    if (dotted == d.array_path) return true;
  }
  return false;
}

// The catalogue entry for one (array, name) pair, or nullptr for a component
// the engine does not build. The UI resolves rows through this, which is why a
// row no longer depends on an array index surviving a merge: the loader looks
// entries up by `name` (StrategyFactory::Build, BuildExitPlan), so the menu
// does too.
inline const ComponentDef* FindComponent(const std::string& array_path,
                                         const std::string& name) {
  for (const ComponentDef& d : Catalogue()) {
    if (array_path == d.array_path && name == d.name) return &d;
  }
  return nullptr;
}

// The full shape: every table field at its default, every catalogue component
// present and DISABLED.
//
// Disabled is why a brand-new profile is not yet runnable: Validate() rejects a
// trigger list in which nothing is active, because a run with no possible
// signal is indistinguishable from a strategy that found no setups. That is
// reported by the editor's status line, and it is the ONLY thing a fresh
// profile still needs from you -- one trigger armed.
inline nlohmann::json Skeleton() {
  using nlohmann::json;
  json s = json::object();
  for (const Field& f : Schema()) {
    s[PointerFor(f.path)] = f.default_value;
  }
  for (const ComponentDef& d : Catalogue()) {
    json entry = json::object();
    entry["name"] = d.name;
    // the factory's IsDisabled accepts enabled/active/isActive; the catalogue
    // writes the canonical one so the file says "off" explicitly rather than
    // relying on a missing-flag default.
    entry["enabled"] = false;
    for (auto it = d.params.begin(); it != d.params.end(); ++it) {
      entry[it.key()] = it.value();
    }
    json& arr = s[PointerFor(d.array_path)];
    if (!arr.is_array()) arr = json::array();
    arr.push_back(entry);
  }
  // no table describes the WFA subtree and the factory never reads it (the
  // runner does), so it cannot be enumerated. Present as an empty object: the
  // category exists, its rows appear if a file adds them.
  if (!s.contains("backtest")) s["backtest"] = json::object();
  return s;
}

// The file laid over the skeleton.
//
// Component arrays merge BY NAME, not by index: a profile that writes one
// trigger must not erase the other catalogue entries from the menu, and array
// order is incidental to the loader -- the factory resolves every entry by
// `name`, and an index is not an identity.
inline void Overlay(nlohmann::json& base, const nlohmann::json& over,
                    const std::string& dotted = std::string()) {
  using nlohmann::json;

  if (IsComponentPath(dotted) && base.is_array()) {
    if (!over.is_array()) return;
    for (const json& item : over) {
      if (!item.is_object() || !item.contains("name") ||
          !item["name"].is_string()) {
        base.push_back(item);
        continue;
      }
      const std::string name = item["name"].get<std::string>();
      auto match = std::find_if(
          base.begin(), base.end(), [&name](const json& b) {
            return b.is_object() && b.contains("name") &&
                   b["name"].is_string() &&
                   b["name"].get<std::string>() == name;
          });
      if (match != base.end()) {
        Overlay(*match, item, dotted + "." + name);
      } else {
        // a component the catalogue does not know: keep it, and disable it by
        // default so a stale name cannot arm by accident.
        json entry = item;
        if (!entry.contains("enabled") && !entry.contains("active") &&
            !entry.contains("isActive")) {
          entry["enabled"] = false;
        }
        base.push_back(entry);
      }
    }
    return;
  }

  if (base.is_object() && over.is_object()) {
    for (auto it = over.begin(); it != over.end(); ++it) {
      const std::string child =
          dotted.empty() ? it.key() : dotted + "." + it.key();
      if (base.contains(it.key())) {
        Overlay(base[it.key()], it.value(), child);
      } else {
        base[it.key()] = it.value();
      }
    }
    return;
  }

  base = over;
}

// Only deviations.
//
// A value set back to its default leaves the file, so the profile stays a diff
// and a later table-default change still reaches it. Two exceptions, both
// forced by AppConfig::Load rather than chosen here:
//
//  - kRequired fields are always written. The loader has no fallback for them
//    (D7), so omitting one produces a profile that cannot start.
//  - `triggers` is always written, including its disabled entries, because
//    Validate() rejects an empty list. Writing them also keeps the message
//    useful: "every entry in 'triggers' is disabled" names the thing to fix.
//
// Disabled filters and risk modules are omitted entirely -- an empty array is
// legal for both, so there is nothing to write.
inline nlohmann::json Diff(const nlohmann::json& cur,
                           const nlohmann::json& base,
                           const std::string& dotted = std::string()) {
  using nlohmann::json;

  if (IsComponentPath(dotted) && cur.is_array()) {
    json out = json::array();
    for (const json& item : cur) {
      if (!item.is_object() || !item.contains("name") ||
          !item["name"].is_string()) {
        // an unnamed entry cannot be addressed by the loader either; keep it
        // verbatim rather than deleting the user's data on save.
        out.push_back(item);
        continue;
      }
      const std::string name = item["name"].get<std::string>();
      const json* match = nullptr;
      if (base.is_array()) {
        for (const json& b : base) {
          if (b.is_object() && b.contains("name") && b["name"].is_string() &&
              b["name"].get<std::string>() == name) {
            match = &b;
            break;
          }
        }
      }
      json d = match != nullptr ? Diff(item, *match, dotted + "." + name)
                                : item;

      const bool enabled =
          item.contains("enabled") && item["enabled"].is_boolean()
              ? item["enabled"].get<bool>()
              : true;  // no flag means enabled, matching the factory
      if (dotted != "triggers" && !enabled) continue;

      if (!d.is_object()) d = json::object();
      d["name"] = name;  // an entry is addressed by name, not by index
      if (item.contains("enabled")) d["enabled"] = item["enabled"];
      out.push_back(d);
    }
    return out.empty() ? json() : out;
  }

  // The structural-exit block is written WHOLE the moment the master switch is
  // on. It is the one place where a diff produces a profile that cannot load:
  // ResolveStructuralExitConfig requires min_holdtime_ms whenever `enabled` is
  // true, and that key is at its table default until someone edits it -- so a
  // diff would omit it and the saved profile would fail on load. Writing the
  // block verbatim costs a few redundant keys and cannot break that contract.
  if (dotted == "trade_management.structural_exit" && cur.is_object() &&
      cur.value("enabled", false)) {
    return cur;
  }

  if (cur.is_object() && base.is_object()) {
    json out = json::object();
    for (auto it = cur.begin(); it != cur.end(); ++it) {
      const std::string child =
          dotted.empty() ? it.key() : dotted + "." + it.key();
      const json d = base.contains(it.key())
                         ? Diff(it.value(), base[it.key()], child)
                         : it.value();
      const Field* f = Find(child);
      const bool required = f != nullptr && (f->flags & kRequired) != 0;

      if (d.is_null() || (d.is_object() && d.empty()) ||
          (d.is_array() && d.empty())) {
        if (required) out[it.key()] = it.value();
        continue;
      }
      out[it.key()] = d;
    }
    return out.empty() ? json() : out;
  }

  return cur == base ? json() : cur;
}

}  // namespace mokuren::cfg
