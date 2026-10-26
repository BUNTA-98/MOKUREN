#include "runner_common.hpp"

#include "config_parser.hpp"
#include "factory.hpp"
#include "logger.hpp"
#include "trade_management.hpp"

#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace mokuren {

void ValidateTradeManagement(const nlohmann::json& cfg) {
    if (!cfg.contains("trade_management") ||
        !cfg["trade_management"].is_object()) {
        throw std::runtime_error(
            "config error: missing required section 'trade_management'");
    }
    const auto& tm = cfg["trade_management"];
    for (const char* section : {"break_even", "trailing", "scale_out"}) {
        if (!tm.contains(section) || !tm[section].is_object()) {
            throw std::runtime_error(
                std::string("config error: missing required section "
                            "'trade_management.") + section + "'");
        }
    }
}

void ValidateRunConfig(const nlohmann::json& cfg, const char* where,
                       bool require_venue_credentials) {
    try {
        AppConfig::Load(cfg, require_venue_credentials);
        mokuren::ResolveRiskPcts(cfg);
        ValidateTradeManagement(cfg);
    } catch (const std::exception& e) {
        LOG_ERROR("{}: invalid config: {}", where, e.what());
        throw;
    }
}

unsigned int ReadMaxCores(const nlohmann::json& cfg) {
    unsigned int n = 0;
    if (cfg.contains("environment") && cfg["environment"].is_object()) {
        const auto& env = cfg["environment"];
        if (env.contains("max_cores")) {
            const auto& v = env["max_cores"];
            if (v.is_number_integer() && v.get<long long>() > 0) {
                n = static_cast<unsigned int>(v.get<long long>());
            } else if (v.is_string()) {
                const std::string s = v.get<std::string>();
                size_t used = 0;
                try {
                    const unsigned long parsed = std::stoul(s, &used);
                    if (used == s.size() && parsed > 0) {
                        n = static_cast<unsigned int>(parsed);
                    }
                } catch (...) {
                    n = 0;
                }
            }
        }
    }
    if (n == 0) {
        n = std::thread::hardware_concurrency();
        if (n == 0) n = 4;
    }
    return n;
}

void ApplyRMultiplesManagement(const nlohmann::json& json_config, double base_sl_pct, EngineInstance& eng) {
    if (!(base_sl_pct > 0.0)) {
        throw std::runtime_error(
            "ApplyRMultiplesManagement: sl_pct must be > 0; it defines 1R");
    }
    ValidateTradeManagement(json_config);
    const auto& tm = json_config["trade_management"];
    const auto& be = tm["break_even"];
    const auto& tr = tm["trailing"];
    const auto& so = tm["scale_out"];

    // Nothing owns the r-multiple ladder if pos_manager is absent, and falling
    // through silently disabled the paper broker's management too: the trade
    // would keep its stop and target but never break even, trail or scale out.
    if (!eng.pos_manager) {
        throw std::runtime_error(
            "ApplyRMultiplesManagement: engine has no PositionManager; "
            "break-even/trailing/scale-out would never fire");
    }

    // PaperTrader executes the price-based stop and target only. its own
    // break-even / trailing / scale-out run on percentages of entry, while the
    // PositionManager ladder runs on the stop that actually gets placed, so
    // running both moved the same stop twice and could scale out twice.
    if (auto* pt = dynamic_cast<PaperTrader*>(eng.ptrader.get())) {
        pt->DisableManagement();
    }

    // ResolveLevelPct returns pct-of-entry (r * sl_pct); dividing recovers the
    // r the author wrote, measured against the placed stop.
    auto level_r = [base_sl_pct](const nlohmann::json& block, const char* r_key,
                                 const char* pct_key, const char* section) {
        return mokuren::ResolveLevelPct(block, base_sl_pct, r_key, pct_key,
                                        section) / base_sl_pct;
    };
    // mirrors AppConfig::Load's SafeBool. Load accepts "false"/0 as well as a
    // real false for these flags, so the strict is_boolean() check this
    // replaced rejected configs the loader had already accepted -- and it did
    // so inside a worker, where the throw surfaced as "no trades" instead of as
    // a config error. an absent flag means enabled, matching the loader and
    // StrategyFactory::Build.
    auto read_enabled = [](const nlohmann::json& block) {
        if (!block.contains("enabled")) return true;
        const auto& v = block["enabled"];
        if (v.is_boolean()) return v.get<bool>();
        if (v.is_string()) {
            const std::string s = v.get<std::string>();
            if (s == "true" || s == "True" || s == "TRUE" || s == "1") return true;
            if (s == "false" || s == "False" || s == "FALSE" || s == "0") return false;
            return true;
        }
        if (v.is_number_integer()) return v.get<long long>() != 0;
        if (v.is_number_float()) return v.get<double>() != 0.0;
        return true;
    };
        
    const bool trailing_on = read_enabled(tr);
    const bool scale_out_on = read_enabled(so);
    // AppConfig::Load accepts "fraction": "0.5" (RequireNumber parses numeric
    // strings), so demanding is_number() here rejected a config the loader had
    // already approved -- inside a worker, where it surfaced as "no trades".
    auto read_number = [](const nlohmann::json& block, const char* key,
                          const char* path) -> double {
        if (!block.contains(key)) {
            throw std::runtime_error(std::string("config error: missing required key '") +
                                     path + "'");
        }
        const auto& v = block[key];
        if (v.is_number()) return v.get<double>();
        if (v.is_string()) {
            const std::string s = v.get<std::string>();
            size_t used = 0;
            try {
                const double parsed = std::stod(s, &used);
                if (used == s.size()) return parsed;
            } catch (...) {
                // fall through to the typed error
            }
        }
        throw std::runtime_error(std::string("config error: '") + path +
                                 "' must be a number (got " + v.dump() + ")");
    };
    const double scale_fraction =
        read_number(so, "fraction", "trade_management.scale_out.fraction");

    // an explicit "enabled": false on break-even is honoured by pushing the
    // trigger out of reach. Resolve the levels only when the block is active: a
    // deliberately disabled block must not be forced to carry trigger keys the
    // author omitted, or the whole run aborts for a feature they turned off.
    double be_trigger_r = std::numeric_limits<double>::infinity();
    double be_target_r = 0.0;
    // the same reader the other two flags use, so a break-even block disabled
    // with "false" is honoured exactly like one disabled with false.
    const bool be_disabled = !read_enabled(be);
    if (!be_disabled) {
        be_trigger_r =
            level_r(be, "trigger_r", "trigger_pct", "trade_management.break_even");
        be_target_r =
            level_r(be, "target_r", "target_pct", "trade_management.break_even");
    }


    eng.pos_manager->Configure(
        be_trigger_r, be_target_r, trailing_on,
        level_r(tr, "trigger_r", "trigger_pct", "trade_management.trailing"),
        level_r(tr, "distance_r", "distance_pct", "trade_management.trailing"),
        scale_out_on,
        level_r(so, "trigger_r", "trigger_pct", "trade_management.scale_out"),
        scale_fraction);
}

} // namespace mokuren
