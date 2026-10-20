#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include "config_parser.hpp"

using json = nlohmann::json;

TEST_CASE("Config Parser Basics", "[config]") {
    // Load is strict: sections and any key that changes what a run does must be
    // written out. this test used to assert defaults for missing sections,
    // which is exactly the behaviour RequireNumber/RequireBool removed.
    SECTION("AppConfig rejects a config without the required sections") {
        json empty_json = R"({"triggers": [{"name": "OrderbookImbalance", "ratio": 3.0}]})"_json;
        REQUIRE_THROWS_AS(AppConfig::Load(empty_json), std::runtime_error);
        
    }

    SECTION("AppConfig parses nested JSON correctly") {
        // a complete, valid config: every required section and every required
        // key, with the levels expressed in R so sl_pct defines 1R.
        // the trigger is a real factory name: "OrderbookImbalance" is a
        // filter, and Load never interprets names, so a typo here would only
        // surface as a StrategyFactory abort long after this test passes.
        json custom_json = R"({
            "exchange": "binance",
            "environment": {
                "interval_ms": 60000,
                "macro_interval_ms": 900000,
                "tick_size": 0.5,
                "vwap_reset_hour": 0
            },
            "execution": {"taker_fee_pct": 0.0004, "slippage_pct": 0.0002},
            "triggers": [{"name": "StackedImbalance", "ratio": 3.0}],
            "risk": {
                "sl_pct": 0.01,
                "tp_r": 2.0,
                "risk_per_trade_pct": 0.01,
                "max_daily_loss": 500.0
            },
            "trade_management": {
                "break_even": {"trigger_r": 1.0, "target_r": 0.2},
                "trailing": {"enabled": true, "trigger_r": 1.5, "distance_r": 0.5},
                "scale_out": {"enabled": false, "trigger_r": 0.8, "fraction": 0.5}
            }
        })"_json;
        
        AppConfig cfg = AppConfig::Load(custom_json);
        
        REQUIRE(cfg.exchange == "binance");
        REQUIRE(cfg.tick_size == 0.5);
        REQUIRE(cfg.sl_pct == 0.01);
        REQUIRE(cfg.max_daily_loss == 500.0);
        // tp_r is a multiple of sl_pct (1R), so 2R of a 1% stop is 2%.
        REQUIRE(cfg.tp_pct == 0.02);
        REQUIRE(cfg.tm_config.be_trigger_pct == 0.01);
    }
}

TEST_CASE("AppConfig struct defaults are unchanged", "[config]") {
    // defaults of a directly constructed AppConfig (tests, tooling). Load
    // never applies them: it requires every key that changes a run. these are
    // the four assertions the old "applies defaults for missing keys" section
    // made against Load, restored here against the struct that still owns them.
    const AppConfig cfg{};

    REQUIRE(cfg.exchange == "binance");
    REQUIRE(cfg.interval_ms == 60000);
    REQUIRE(cfg.macro_interval_ms == 900000);
    REQUIRE(cfg.vwap_reset_hour == 0);
    REQUIRE(cfg.tick_size == 1.0);
    REQUIRE(cfg.max_cores == 0u);

    REQUIRE(cfg.taker_fee_pct == 0.0004);
    REQUIRE(cfg.slippage_pct == 0.0002);

    REQUIRE(cfg.sl_pct == 0.002);
    REQUIRE(cfg.tp_pct == 0.01);
    REQUIRE(cfg.risk_per_trade_pct == 0.01);
    REQUIRE(cfg.min_distance_dollars == 0.0);
    REQUIRE(cfg.max_daily_loss == 400.0);

    // fail-safe default: a misspelled is_testnet must not route live orders
    REQUIRE(cfg.is_testnet == true);

    REQUIRE(cfg.tm_config.enable_break_even == true);
    REQUIRE(cfg.tm_config.enable_trailing == true);
    REQUIRE(cfg.tm_config.enable_scale_out == false);
    REQUIRE(cfg.tm_config.be_trigger_pct == 0.005);
    REQUIRE(cfg.tm_config.be_target_pct == 0.001);
    REQUIRE(cfg.tm_config.trailing_trigger_pct == 0.008);
    REQUIRE(cfg.tm_config.trailing_dist_pct == 0.004);
    REQUIRE(cfg.tm_config.scale_out_trigger_pct == 0.006);
    REQUIRE(cfg.tm_config.scale_out_fraction == 0.5);
}

TEST_CASE("Grid Scanner Logic", "[grid]") {
    SECTION("GridScanner generates exact step permutations") {
        json grid_config = R"({
            "filters": [
                {
                    "name": "OrderbookImbalance",
                    "ratio": {"min": 2.0, "max": 4.0, "step": 1.0}
                }
            ]
        })"_json;

        auto runs = GridScanner::GenerateGrid(grid_config);
        
        // Erwartet: 2.0, 3.0 und 4.0 -> exakt 3 Durchläufe
        REQUIRE(runs.size() == 3);
        REQUIRE(runs[0].grid_values["ratio"] == 2.0);
        REQUIRE(runs[1].grid_values["ratio"] == 3.0);
        REQUIRE(runs[2].grid_values["ratio"] == 4.0);
    }
}
