#include "venue_spec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

using mokuren::venue::Env;
using mokuren::venue::Resolve;

TEST_CASE("venue: bybit demo resolves to demo hosts", "[venue]") {
  const auto spec = Resolve("bybit", Env::Demo, "linear", "BTCUSDT");
  REQUIRE(spec.venue == "bybit");
  REQUIRE(spec.env == Env::Demo);
  REQUIRE(spec.rest_base == "https://api-demo.bybit.com");
  REQUIRE(spec.public_ws == "wss://stream-demo.bybit.com/v5/public/linear");
  REQUIRE(spec.private_ws == "wss://stream-demo.bybit.com/v5/private");
}

TEST_CASE("venue: blofin demo resolves to demo hosts", "[venue]") {
  const auto spec = Resolve("blofin", Env::Demo, "linear", "BTC-USDT");
  REQUIRE(spec.venue == "blofin");
  REQUIRE(spec.env == Env::Demo);
  REQUIRE(spec.rest_base == "https://demo-trading-openapi.blofin.com");
  REQUIRE(spec.public_ws == "wss://demo-trading-openapi.blofin.com/ws/public");
  REQUIRE(spec.private_ws == "wss://demo-trading-openapi.blofin.com/ws/private");
}

TEST_CASE("venue: blofin mainnet resolves to mainnet hosts", "[venue]") {
  const auto spec = Resolve("blofin", Env::Mainnet, "linear", "BTC-USDT");
  REQUIRE(spec.rest_base == "https://openapi.blofin.com");
  REQUIRE(spec.public_ws == "wss://openapi.blofin.com/ws/public");
}

TEST_CASE("venue: blofin has no testnet", "[venue]") {
  REQUIRE_THROWS_AS(Resolve("blofin", Env::Testnet, "linear", "BTC-USDT"),
                    std::invalid_argument);
}

TEST_CASE("venue: unknown venue throws", "[venue]") {
  REQUIRE_THROWS_AS(Resolve("kraken", Env::Demo, "linear", "BTC-USDT"),
                    std::invalid_argument);
}

TEST_CASE("venue: unknown category throws", "[venue]") {
  REQUIRE_THROWS_AS(Resolve("bybit", Env::Demo, "futures", "BTCUSDT"),
                    std::invalid_argument);
}

TEST_CASE("venue: empty symbol throws", "[venue]") {
  REQUIRE_THROWS_AS(Resolve("bybit", Env::Demo, "linear", ""),
                    std::invalid_argument);
}
