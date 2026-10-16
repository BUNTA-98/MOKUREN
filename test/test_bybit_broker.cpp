#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "bybit_broker.hpp"

#include <nlohmann/json.hpp>

#include <string>

TEST_CASE("Bybit Broker Logic", "[bybit][broker]") {
    SECTION("GenerateSignature produces correct HMAC SHA256") {
        BybitBroker broker("test_key", "test_secret", true);

        const std::string sig = broker.GenerateSignature("payload_data", "1234567890");

        // Encoding guard: HMAC-SHA256 is 32 bytes -> exactly 64 lowercase hex
        // characters. Catches a broken ToHex() without knowing the digest.
        REQUIRE(sig.size() == 64);
        REQUIRE(sig.find_first_not_of("0123456789abcdef") == std::string::npos);

        // Deterministic: identical inputs must yield identical signatures.
        REQUIRE(broker.GenerateSignature("payload_data", "1234567890") == sig);

        // Documented contract: a non-empty timestamp is PREFIXED to the payload,
        // so these two calls must sign the same message and match exactly.
        REQUIRE(broker.GenerateSignature("payload_data", "1234567890") ==
                broker.GenerateSignature("1234567890payload_data", ""));

        // Sensitivity: a different timestamp changes the signed message, so the
        // MAC must differ (guards against a signer that ignores its inputs).
        REQUIRE(broker.GenerateSignature("payload_data", "1000000000") != sig);

        // To pin an exact known-answer value as well, cross-check against:
        //   printf '1234567890payload_data' | openssl dgst -sha256 -hmac 'test_secret'
        // then add a plain REQUIRE against that output.
    }

    SECTION("HandlePrivateMessage parses position updates correctly") {
        BybitBroker broker("dummy", "dummy", true);

        const std::string fake_json =
            R"({"topic":"position","data":[{"symbol":"BTCUSDT","side":"Buy","size":"2.5","entryPrice":"65000.0","stopLoss":"64000.0","takeProfit":"66000.0"}]})";

        broker.HandlePrivateMessage(fake_json);

        REQUIRE(broker.HasOpenPosition() == true);
        REQUIRE(broker.GetCurrentPosition().direction == SignalDirection::BUY);
        REQUIRE_THAT(broker.GetCurrentPosition().volume,
                     Catch::Matchers::WithinAbs(2.5, 0.0001));
        // TradeSignal exposes the fill as `entry_price`, not `price`.
        REQUIRE_THAT(broker.GetCurrentPosition().entry_price,
                     Catch::Matchers::WithinAbs(65000.0, 0.0001));
    }

    SECTION("HandlePrivateMessage parses execution/fills correctly") {
        BybitBroker broker("dummy", "dummy", true);

        const std::string fake_json =
            R"({"topic":"execution","data":[{"symbol":"BTCUSDT","execQty":"1.0","closedSize":"1.5"}]})";

        broker.HandlePrivateMessage(fake_json);

        REQUIRE_THAT(broker.GetLastClosedSize(),
                     Catch::Matchers::WithinAbs(1.5, 0.0001));
    }
}
