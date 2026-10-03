#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include "config_parser.hpp"

using json = nlohmann::json;

TEST_CASE("Config Parser Basics", "[config]") {
    SECTION("AppConfig loads default fallback values") {
        json empty_json = json::object();
        AppConfig cfg = AppConfig::Load(empty_json);
        
        REQUIRE(cfg.interval_ms == 60000);
        REQUIRE(cfg.sl_pct == 0.002);
        REQUIRE(cfg.max_daily_loss == 400.0);
        REQUIRE(cfg.vwap_reset_hour == 0);
    }

    SECTION("AppConfig parses nested JSON correctly") {
        json custom_json = R"({
            "environment": {"tick_size": 0.5},
            "risk": {"sl_pct": 0.01, "max_daily_loss": 500.0}
        })"_json;
        
        AppConfig cfg = AppConfig::Load(custom_json);
        
        REQUIRE(cfg.tick_size == 0.5);
        REQUIRE(cfg.sl_pct == 0.01);
        REQUIRE(cfg.max_daily_loss == 500.0);
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