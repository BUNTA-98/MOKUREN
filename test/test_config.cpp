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
        // hyperliquid, not bybit: Load() runs with the default
        // require_venue_credentials=true, and bybit then refuses a blank
        // api_key. binance is no longer an accepted venue at all (its depth-1
        // book cannot feed the depth triggers), so naming it here failed
        // Validate() before a single assertion ran.
        //
        // NOTE: the comment above is C++, not JSON. nlohmann has no comment
        // syntax, so a "//" line inside the raw literal is a parse error at
        // the first slash -- which is exactly how this test failed.
        json custom_json = R"({
            "exchange": "hyperliquid",
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
        
        REQUIRE(cfg.exchange == "hyperliquid");
        REQUIRE(cfg.tick_size == 0.5);
        REQUIRE(cfg.sl_pct == 0.01);
        REQUIRE(cfg.max_daily_loss == 500.0);
        // tp_r is a multiple of sl_pct (1R), so 2R of a 1% stop is 2%.
        REQUIRE(cfg.tp_pct == 0.02);
        REQUIRE(cfg.tm_config.be_trigger_pct == 0.01);
    }
}

TEST_CASE("AppConfig struct defaults hold apart from the venue", "[config]") {
    // defaults of a directly constructed AppConfig (tests, tooling). Load
    // never applies them: it requires every key that changes a run. these are
    // the four assertions the old "applies defaults for missing keys" section
    // made against Load, restored here against the struct that still owns them.
    const AppConfig cfg{};

    // the venue is the one default that changed: it is now empty, so a
    // directly-constructed AppConfig cannot silently stream a venue nobody
    // selected. the failure that produces is pinned by the case below.
    REQUIRE(cfg.exchange.empty());
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

TEST_CASE("a default-constructed AppConfig refuses to validate without a venue",
          "[config]") {
    // this is the behaviour the empty exchange default exists for. a default
    // that named a venue is how a config-less caller (tests, tooling) ended up
    // streaming binance data for an account configured elsewhere.
    const AppConfig cfg{};

    REQUIRE_THROWS_AS(cfg.Validate(/*require_venue_credentials=*/false),
                      std::invalid_argument);

    // false above: the venue check must not depend on credentials, or a shadow
    // run would get past it. assert the message names the venue so a future
    // failure here is not mistaken for one of the other Validate() rules.
    try {
        cfg.Validate(/*require_venue_credentials=*/false);
        FAIL("Validate() accepted an AppConfig with no exchange");
    } catch (const std::invalid_argument& e) {
        REQUIRE(std::string(e.what()).find("exchange") != std::string::npos);
    }
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
