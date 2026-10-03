#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

TEST_CASE("Engine Math Sanity Check", "[math]") {
    
    SECTION("Basic PnL Calculation") {
        double entry = 60000.0;
        double exit = 60600.0;
        double volume = 0.5; 
        
        double pnl = (exit - entry) * volume;

        REQUIRE_THAT(pnl, Catch::Matchers::WithinAbs(300.0, 0.00001));
    }

    SECTION("Engine Timestamp Conversion") {
        int64_t timestamp_ms = 1711929600000; 
        int64_t seconds = timestamp_ms / 1000;
        
        REQUIRE(seconds == 1711929600);
    }
}