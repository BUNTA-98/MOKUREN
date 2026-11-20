#pragma once
#include "config_schema.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace mokuren {

// The dataset a profile names, with the table's default applied the same way
// AppConfig::Load applies it.
//
// The runners used to read environment.dataset straight out of the raw json
// while Load resolved it through the schema: two owners, one value, and they
// disagreed for every profile the editor writes. The TUI saves a DIFF, so a key
// left at its default is omitted from the file -- `environment.dataset` has no
// on-disk presence in a normal profile, and the raw read answered "no dataset
// configured" for a config whose Load-resolved value was "data/". Same profile,
// two answers, depending on which path looked.
//
// One owner now. Every driver, every replay entry point and the scanner call
// this, so a sparse profile and a hand-written one mean the same thing wherever
// they are read.
inline std::string DatasetFromJson(const nlohmann::json& src) {
    const cfg::Field* def = cfg::Find("environment.dataset");
    std::string fallback;
    if (def != nullptr && def->default_value.is_string()) {
        fallback = def->default_value.get<std::string>();
    }

    const nlohmann::json* v = cfg::Lookup(src, "environment.dataset");
    if (v == nullptr) return fallback;
    if (v->is_string()) return v->get<std::string>();
    // A grid node: the value a run would use is its `val`, which is what
    // FlattenGrids writes and what cfg::GetString returns.
    if (v->is_object() && v->contains("val") && (*v)["val"].is_string()) {
        return (*v)["val"].get<std::string>();
    }
    return fallback;
}

}  // namespace mokuren
