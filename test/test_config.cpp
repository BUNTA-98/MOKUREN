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
        // this fixture deliberately still carries risk.max_daily_loss, a key
        // Load no longer reads. reaching this assertion at all is the
        // compatibility guarantee made when that ladder was deleted: an old
        // config loads unchanged, and the key is ignored rather than rejected.
        //
        // what the account's limit is now lives under "prop", and this fixture
        // has no such block -- so the guard is inert here. it is NOT silently
        // armed off the dead dollar amount, which is the failure the presence
        // rule below is written to rule out.
        REQUIRE_FALSE(cfg.prop.enabled);
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
    // a directly-constructed AppConfig carries an INERT prop guard: the numbers
    // are the firm's, but nothing enforces them until a config's "prop" block
    // arms them. the old max_daily_loss default of 400.0 had no opt-out and,
    // being an absolute amount, scaled with nothing -- not with the account,
    // not with the rule.
    REQUIRE_FALSE(cfg.prop.enabled);
    REQUIRE(cfg.prop.max_daily_drawdown_pct == 0.04);
    REQUIRE(cfg.prop.max_total_drawdown_pct == 0.06);

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

TEST_CASE("a prop block arms the account limits by its presence", "[config][prop]") {
    // every key that changes what a run does is still required; only the prop
    // block varies below. hyperliquid with require_venue_credentials=false, so
    // the fixture needs no api key and no venue block.
    auto load_with_prop = [](const json& prop_block) {
        json cfg = R"({
            "exchange": "hyperliquid",
            "environment": {
                "interval_ms": 60000,
                "macro_interval_ms": 900000,
                "tick_size": 0.5,
                "vwap_reset_hour": 0
            },
            "execution": {"taker_fee_pct": 0.0004, "slippage_pct": 0.0002},
            "initial_balance": 5000.0,
            "triggers": [{"name": "StackedImbalance", "ratio": 3.0}],
            "risk": {"sl_pct": 0.01, "tp_r": 2.0, "risk_per_trade_pct": 0.01},
            "trade_management": {
                "break_even": {"trigger_r": 1.0, "target_r": 0.2},
                "trailing": {"enabled": true, "trigger_r": 1.5, "distance_r": 0.5},
                "scale_out": {"enabled": false, "trigger_r": 0.8, "fraction": 0.5}
            }
        })"_json;
        if (prop_block.is_null()) {
            cfg.erase("prop");
        } else {
            cfg["prop"] = prop_block;
        }
        return AppConfig::Load(cfg, /*require_venue_credentials=*/false);
    };

    SECTION("an absent block leaves the guard inert") {
        // the only way to say "no account limits" now, which is why a backtest
        // config that means to enforce them has to write the block out.
        const AppConfig cfg = load_with_prop(json());
        REQUIRE_FALSE(cfg.prop.enabled);
    }

    SECTION("presence alone arms it and its values win") {
        // the semantics this pins: a block that omitted "enabled" used to parse
        // as OFF, so a config that looked like it configured the firm's limits
        // enforced nothing and reported nothing. presence is now the opt-in,
        // and these are the numbers the guard actually measures against.
        const AppConfig cfg = load_with_prop(json{
            {"max_daily_drawdown_pct", 0.03},
            {"max_total_drawdown_pct", 0.08},
            {"warn_fraction", 0.5},
            {"daily_reset_hour_utc", 7},
            {"flatten_on_breach", false},
        });
        REQUIRE(cfg.prop.enabled);
        REQUIRE(cfg.prop.max_daily_drawdown_pct == 0.03);
        REQUIRE(cfg.prop.max_total_drawdown_pct == 0.08);
        REQUIRE(cfg.prop.warn_fraction == 0.5);
        REQUIRE(cfg.prop.daily_reset_hour_utc == 7);
        REQUIRE_FALSE(cfg.prop.flatten_on_breach);
    }

    SECTION("an explicit false still turns it off") {
        // the one escape hatch left. a run that wants the block documented but
        // inactive (a template, a shared config) says so here.
        const AppConfig cfg = load_with_prop(json{{"enabled", false}});
        REQUIRE_FALSE(cfg.prop.enabled);
    }

    SECTION("arming the guard is what makes its limits checked") {
        // 5.0 is 500%, not a fraction. this only throws because the block armed
        // the guard -- which is the point of the presence rule: a typo in a
        // block that looks configured can no longer hide behind a guard that
        // never ran.
        REQUIRE_THROWS_AS(
            load_with_prop(json{{"max_daily_drawdown_pct", 5.0}}),
            std::runtime_error);
        // the same number is tolerated when the guard is explicitly off, so a
        // config carrying the block for the other fields stays loadable.
        const AppConfig off =
            load_with_prop(json{{"enabled", false},
                                {"max_daily_drawdown_pct", 5.0}});
        REQUIRE_FALSE(off.prop.enabled);
    }

    SECTION("prop.initial_balance is a hard error, not a second base") {
        // accepting it here is how total drawdown once got measured against a
        // number the account never had. the base is account state and travels
        // as initial_balance, so naming it must fail rather than be ignored.
        REQUIRE_THROWS_AS(
            load_with_prop(json{{"initial_balance", 5000.0}}),
            std::runtime_error);
    }
}
