// Component-level config audit. AppConfig::Load treats triggers, filters and
// risk modules as opaque json, so it validates neither their names nor the
// params the factory reads. These tests pin the factory's whole surface: every
// supported name, the disable spellings, the tolerant param readers, and the
// fail-fast paths. src/factory.cpp is the source of truth; keep the lists here
// in sync with the doc block in include/factory.hpp.
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/common.h>
#include <stdexcept>
#include <string>

#include "config_parser.hpp"
#include "factory.hpp"
#include "logger.hpp"

using json = nlohmann::json;

namespace {

// Build logs through the global async logger. initialise it once so these
// tests do not depend on another TU having done it first; "off" keeps the
// output clean.
struct EnsureLogger {
  EnsureLogger() {
    mokuren::InitLogger("logs/engine_tests.log", spdlog::level::off);
  }
};
const EnsureLogger kEnsureLogger;

// A config Load accepts AND Build can wire. The trigger is a real factory
// name: Validate only checks that names are non-empty, so a typo passes Load
// and aborts in Build.
//
// the venue is hyperliquid on purpose: Load() runs with the default
// require_venue_credentials=true, and bybit would then demand an api_key,
// which has nothing to do with what these cases test. binance is no longer an
// accepted venue -- its depth-1 book cannot feed the depth triggers -- so it
// failed Validate() before any wiring assertion ran.
json BaseConfig() {
  return json::parse(R"({
    "exchange": "hyperliquid",
    "environment": {
      "interval_ms": 60000,
      "macro_interval_ms": 900000,
      "tick_size": 0.5,
      "vwap_reset_hour": 0
    },
    "execution": {"taker_fee_pct": 0.0004, "slippage_pct": 0.0002},
    "triggers": [{"name": "StackedImbalance", "ratio": 1.5, "levels": 2}],
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
  })");
}

EngineInstance BuildFrom(const json& j) {
  return StrategyFactory::Build(AppConfig::Load(j));
}

} // namespace

TEST_CASE("Factory: every supported component name resolves", "[factory]") {
  SECTION("triggers") {
    for (const char* name :
         {"DeltaAbsorption", "StackedImbalance", "SpoofHunter"}) {
      json j = BaseConfig();
      j["triggers"] = json::array({json::object({{"name", name}})});
      INFO("trigger: " << name);
      CHECK_NOTHROW(BuildFrom(j));
    }
  }

  SECTION("filters") {
    for (const char* name :
         {"MinVolume", "VwapTrend", "CVDDivergence", "MacroTrend",
          "Volatility", "TimeOfDay", "POCTrend", "RVOL", "ATRChop",
          "OrderbookImbalance"}) {
      json j = BaseConfig();
      j["filters"] = json::array({json::object({{"name", name}})});
      INFO("filter: " << name);
      CHECK_NOTHROW(BuildFrom(j));
    }
  }

  SECTION("risk modules") {
    for (const char* name : {"SinglePositionLock", "MaxStopDistanceLock",
                             "MaxLeverageLock", "AntiRevengeLock"}) {
      json j = BaseConfig();
      j["risk"]["modules"] = json::array({json::object({{"name", name}})});
      INFO("risk module: " << name);
      CHECK_NOTHROW(BuildFrom(j));
    }
  }
}

TEST_CASE("Factory: unknown names abort before the run starts", "[factory]") {
  SECTION("unknown trigger") {
    json j = BaseConfig();
    j["triggers"] = json::array({json::object({{"name", "NoSuchTrigger"}})});
    REQUIRE_THROWS_AS(BuildFrom(j), std::runtime_error);
  }

  SECTION("OrderbookImbalance is a filter, not a trigger") {
    // the name an earlier test used as a trigger. Load accepts it; Build does
    // not, which is the whole reason this file exists.
    json j = BaseConfig();
    j["triggers"] =
        json::array({json::object({{"name", "OrderbookImbalance"}})});
    REQUIRE_THROWS_AS(BuildFrom(j), std::runtime_error);

    json as_filter = BaseConfig();
    as_filter["filters"] =
        json::array({json::object({{"name", "OrderbookImbalance"}})});
    REQUIRE_NOTHROW(BuildFrom(as_filter));
  }

  SECTION("unknown filter") {
    json j = BaseConfig();
    j["filters"] = json::array({json::object({{"name", "NoSuchFilter"}})});
    REQUIRE_THROWS_AS(BuildFrom(j), std::runtime_error);
  }

  SECTION("unknown risk module") {
    // dropping a risk module by typo removes a safety lock, so this must not
    // degrade to a no-op
    json j = BaseConfig();
    j["risk"]["modules"] =
        json::array({json::object({{"name", "NoSuchLock"}})});
    REQUIRE_THROWS_AS(BuildFrom(j), std::runtime_error);
  }
}

TEST_CASE("Factory: params are optional and tolerant", "[factory]") {
  SECTION("a trigger without any params uses its defaults") {
    for (const char* name :
         {"DeltaAbsorption", "StackedImbalance", "SpoofHunter"}) {
      json j = BaseConfig();
      j["triggers"] = json::array({json::object({{"name", name}})});
      INFO("trigger: " << name);
      CHECK_NOTHROW(BuildFrom(j));
    }
  }

  SECTION("numeric strings are accepted the way GridScanner writes them") {
    json j = BaseConfig();
    j["triggers"] = json::array({json::object(
        {{"name", "StackedImbalance"}, {"ratio", "2.5"}, {"levels", "3"}})});
    REQUIRE_NOTHROW(BuildFrom(j));

    json j2 = BaseConfig();
    j2["filters"] = json::array({json::object(
        {{"name", "MinVolume"}, {"min_volume", "12.5"}})});
    REQUIRE_NOTHROW(BuildFrom(j2));
  }

  SECTION("a bool param accepts string and integer spellings") {
    json j = BaseConfig();
    j["filters"] = json::array({json::object(
        {{"name", "VwapTrend"}, {"require_trend_alignment", "false"}})});
    REQUIRE_NOTHROW(BuildFrom(j));

    j["filters"] = json::array({json::object(
        {{"name", "VwapTrend"}, {"require_trend_alignment", 0}})});
    REQUIRE_NOTHROW(BuildFrom(j));
  }

  SECTION("an unflattened grid node silently falls back to the default") {
    // the live path is expected to run FlattenGrids first. if it does not, an
    // object param is not an error here -- it takes the default, which is the
    // hazard this pins rather than blesses.
    json j = BaseConfig();
    j["triggers"] = json::array({json::object(
        {{"name", "StackedImbalance"},
         {"ratio", json::object({{"min", 1.0}, {"max", 3.0}, {"step", 1.0}})}})});
    REQUIRE_NOTHROW(BuildFrom(j));
  }
}

TEST_CASE("Factory: disable spellings and wiring", "[factory]") {
  SECTION("a disabled component is skipped, not built") {
    json j = BaseConfig();
    j["triggers"] = json::array(
        {json::object({{"name", "StackedImbalance"}}),
         json::object({{"name", "DeltaAbsorption"}, {"enabled", false}})});

    // one survivor is moved into `trigger` and sub_triggers is cleared (see
    // StrategyFactory::Build), so the count alone proves nothing. assert the
    // collapse here, then use two survivors plus one disabled below to show
    // the disabled entry really was dropped instead of quietly kept.
    EngineInstance one = BuildFrom(j);
    REQUIRE(one.trigger != nullptr);
    REQUIRE(one.sub_triggers.empty());

    json three = BaseConfig();
    three["triggers"] = json::array(
        {json::object({{"name", "StackedImbalance"}}),
         json::object({{"name", "DeltaAbsorption"}}),
         json::object({{"name", "SpoofHunter"}, {"enabled", false}})});

    EngineInstance eng = BuildFrom(three);
    REQUIRE(eng.trigger != nullptr);
    REQUIRE(eng.sub_triggers.size() == 2);  // the two enabled ones
  }

  SECTION("every disable spelling is honoured") {
    for (const json& off : {json(false), json("false"), json("0"), json(0)}) {
      json j = BaseConfig();
      j["triggers"] = json::array({json::object(
          {{"name", "StackedImbalance"}, {"active", off}})});
      INFO("disabled with: " << off.dump());
      REQUIRE_THROWS_AS(BuildFrom(j), std::runtime_error);
    }
  }

  SECTION("disabling every trigger is a build error") {
    json j = BaseConfig();
    j["triggers"] = json::array({json::object(
        {{"name", "StackedImbalance"}, {"enabled", false}})});
    REQUIRE_THROWS_AS(BuildFrom(j), std::runtime_error);
  }

  SECTION("one trigger is used directly, several become an OR") {
    json one = BaseConfig();
    REQUIRE(BuildFrom(one).sub_triggers.empty());

    json two = BaseConfig();
    two["triggers"] = json::array({json::object({{"name", "StackedImbalance"}}),
                                   json::object({{"name", "DeltaAbsorption"}})});
    EngineInstance eng = BuildFrom(two);
    REQUIRE(eng.trigger != nullptr);
    REQUIRE(eng.sub_triggers.size() == 2);  // children kept alive by the OR
  }
}

TEST_CASE("Factory: engine-level guards", "[factory]") {
  SECTION("sl_pct must be > 0 because it defines 1R") {
    AppConfig cfg = AppConfig::Load(BaseConfig());
    cfg.sl_pct = 0.0;
    REQUIRE_THROWS_AS(StrategyFactory::Build(cfg), std::runtime_error);
  }

  SECTION("a hyperliquid backtest uses the PaperTrader, never the venue") {
    // BaseConfig() is already hyperliquid; spelled out so this case still
    // reads as the venue-specific one if the fixture venue ever changes.
    json j = BaseConfig();
    j["exchange"] = "hyperliquid";

    EngineInstance eng = BuildFrom(j);
    REQUIRE(eng.broker == nullptr);
    REQUIRE(eng.ptrader != nullptr);
  }

  SECTION("a paper run binds every module to the paper account") {
    EngineInstance eng = BuildFrom(BaseConfig());
    REQUIRE(eng.ptrader != nullptr);
    REQUIRE(eng.pos_manager != nullptr);
    REQUIRE(eng.sizer != nullptr);
    REQUIRE(eng.risk_manager != nullptr);
  }
}
