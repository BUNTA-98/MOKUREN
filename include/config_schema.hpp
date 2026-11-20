#pragma once

// One compiled table owns every configurable scalar: its path, type, default,
// stepper bounds, and how the TUI files it. Readers resolve through this table;
// nothing else may hardcode a field name.
//
// Two rules this header exists to enforce (see config.md):
//
//   1. Presence is asked with Has(src, path), never inferred from a resolved
//      value. A resolved value always exists (the default), so it cannot answer
//      "did the author write this?".
//   2. Defaults are NEVER materialized into a config json. The table is a
//      schema, not a merge target; writing defaults back re-arms guards whose
//      meaning depends on absence (config.md 10.1).
//
// min/max are ADVISORY (UI stepper bounds). Numeric validity -- ranges, derived
// values, cross-field relationships -- is enforced in AppConfig::Validate(),
// which already produces the precise messages. Keep one enforcement point.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace mokuren::cfg {

using json = nlohmann::json;

enum class Kind : uint8_t {
  Bool,     // toggled with ENTER
  Int,      // stepper
  Double,   // stepper
  String,   // inline edit
  Enum,     // left/right over enum_values
  DirPath,  // selector over selector_root, directories only
  FilePath, // selector over selector_root, files only
};

enum : uint32_t {
  kRequired  = 1u << 0,  // absent is an error, not the default (D7)
  kSweepable = 1u << 1,  // may hold a grid node instead of a scalar
};

struct Field {
  const char* path;        // dotted, no leading slash: "risk.sl_pct"
  Kind        kind;
  json        default_value;
  const char* category;    // UI column 1
  const char* subcategory; // UI column 2
  const char* label;       // UI column 3
  int         order;       // sort key; also orders categories (smallest wins)
  uint32_t    flags = 0;
  double      min = 0.0;   // advisory only (see header comment)
  double      max = 0.0;
  const char* enum_values = nullptr;   // "one_way|hedge"
  const char* selector_root = nullptr; // "data" / "configs"
};

// Built once on first call; immutable afterwards. Pointer-stable.
const std::vector<Field>& Schema();
const Field* Find(std::string_view path);  // nullptr when unknown

// Resolve src[path] through the table. A sweepable grid node is collapsed to
// its `val`. Absent optional fields return the table default; absent required
// fields throw. Type mismatches throw naming the path and the value received.
double      GetDouble(const json& src, std::string_view path);
int         GetInt(const json& src, std::string_view path);
bool        GetBool(const json& src, std::string_view path);
std::string GetString(const json& src, std::string_view path);

// "did the author write it?" -- ignores defaults. The only correct presence
// test (config.md 10.1).
bool Has(const json& src, std::string_view path);

// Rejects any leaf this table does not own. Component arrays (triggers,
// filters, risk.modules, trade_management.structural_exit.rules) and the
// file-driven `backtest` subtree are exempt: their contents are interpreted by
// name at build time, not here. A key that is neither is a load error --
// profiles are rebuilt against the table, so a stale spelling must stop the
// run instead of half-applying (config.md 10.13).
void RejectUnknownKeys(const json& src);

// How many leaf paths RejectUnknownKeys would reject. Same walk, same rules --
// the TUI shows the count in its footer so an operator can see a stale profile
// before starting a scan. It decides nothing: the throwing form stays the only
// gate (config.md 14 item 4).
std::size_t CountUnknownKeys(const json& src);

// Does this json node describe a sweep? Shape only, no schema lookup: a
// component-array param (triggers[0].min_delta) is a sweep just like a schema
// scalar.
bool IsGridNode(const json& j);

// Is a sweep legal on this field? Schema lookup. Enforced at load and offered
// by the TUI's "make this a sweep".
bool IsSweepableValue(const json& j, const Field& f);

// Dotted-path lookup. nullptr when any segment is absent or not an object.
const json* Lookup(const json& src, std::string_view path);

} // namespace mokuren::cfg
