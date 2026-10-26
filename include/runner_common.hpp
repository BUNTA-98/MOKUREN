#pragma once

#include <nlohmann/json.hpp>

// forward declaration: only a reference is taken, so the header does not drag
// factory.hpp (and with it every aggreator / broker / trigger) into every
// translation unit that needs a pre-flight check.
struct EngineInstance;

namespace mokuren {

// Fails fast on a malformed trade_management block. Used by
// ApplyRMultiplesManagement and, before any worker is spawned, by the scan and
// WFA entry points.
void ValidateTradeManagement(const nlohmann::json& cfg);

// One pre-flight for every path: AppConfig::Load validates what it reads, then
// the risk block and trade_management are checked, so a typo aborts before the
// dataset is loaded instead of surfacing as an empty result set.
//
// require_venue_credentials is false on the backtest paths: they never build a
// venue broker (StrategyFactory::Build only constructs one when live_mode is
// set), so a blank api_key cannot place an order and must not abort a scan.
// every caller that can sign a request keeps the default (true).
void ValidateRunConfig(const nlohmann::json& cfg, const char* where,
                       bool require_venue_credentials = true);

// One reader for environment.max_cores. json::value throws type_error on
// "max_cores": "8", which used to escape on the driver thread before any
// worker started, so a config AppConfig::Load accepts aborted the whole scan.
unsigned int ReadMaxCores(const nlohmann::json& cfg);

// Applies the r-multiple trade management ladder from the config to the
// engine. Shared by the grid, WFA and WFA-replay paths.
void ApplyRMultiplesManagement(const nlohmann::json& json_config,
                               double base_sl_pct, EngineInstance& eng);

} // namespace mokuren
