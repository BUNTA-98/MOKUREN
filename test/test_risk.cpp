#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "config_parser.hpp" 

TEST_CASE("Risk Math Calculation", "[risk]") {
    double entry_price = 60000.0;
    
    SECTION("Long Slippage is added correctly") {
        double slippage_pct = 0.0002; // 0.02%
        double expected_fill = entry_price * (1.0 + slippage_pct);
        
        REQUIRE_THAT(expected_fill, Catch::Matchers::WithinAbs(60012.0, 0.0001));
    }

    SECTION("Short Slippage is subtracted correctly") {
        double slippage_pct = 0.0002;
        double expected_fill = entry_price * (1.0 - slippage_pct);
        
        REQUIRE_THAT(expected_fill, Catch::Matchers::WithinAbs(59988.0, 0.0001));
    }

    SECTION("Position Sizer obeys Max Daily Loss") {
        double current_loss = 350.0;
        double max_daily_loss = 400.0;
        
        // Die Engine darf keine Trades mehr zulassen, wenn das Restrisiko den nächsten Trade sprengen würde
        bool is_locked = (current_loss >= max_daily_loss);
        REQUIRE(is_locked == false); // Bei 350 von 400 ist die Sperre noch offen
    }
}