#include "blofin_broker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <stdexcept>
#include <string>

using mokuren::venue::Env;
using mokuren::venue::Resolve;

namespace {

BloFinBroker MakeOffline() {
    const auto spec = Resolve("blofin", Env::Demo, "linear", "BTCUSDT");
    return BloFinBroker("k", "s", "p", spec,
                        BloFinBroker::OfflineForTests{});
}

} // namespace

TEST_CASE("blofin broker: signature is 88-char base64 of a 64-char hex string",
          "[blofin][broker]") {
    auto b = MakeOffline();
    const std::string sig = b.GenerateSignature(
        "/api/v1/trade/order", "POST", "1700000000000", "nonce-abc",
        R"({"instId":"BTC-USDT"})");
    // HMAC-SHA256 = 32 bytes -> 64 lowercase hex chars -> base64 of 64 ASCII
    // bytes = 88 chars (64 % 3 = 1 -> "==" pad). A signer that base64'd the
    // raw digest emits 44 chars; this size check catches that.
    REQUIRE(sig.size() == 88);
    REQUIRE(sig.back() == '=');
    REQUIRE(sig.find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                "0123456789+/=") == std::string::npos);
}

TEST_CASE("blofin broker: signature is deterministic and input-sensitive",
          "[blofin][broker]") {
    auto b = MakeOffline();
    const std::string a = b.GenerateSignature(
        "/api/v1/trade/order", "POST", "1700000000000", "n", "{}");
    const std::string a2 = b.GenerateSignature(
        "/api/v1/trade/order", "POST", "1700000000000", "n", "{}");
    REQUIRE(a == a2);
    // nonce changes the message
    REQUIRE(b.GenerateSignature("/api/v1/trade/order", "POST",
                                "1700000000000", "n2", "{}") != a);
    // body changes the message
    REQUIRE(b.GenerateSignature("/api/v1/trade/order", "POST",
                                "1700000000000", "n", "{\"x\":1}") != a);
}

TEST_CASE("blofin broker: live ctor refuses non-demo env",
          "[blofin][broker]") {
    const auto mainnet = Resolve("blofin", Env::Mainnet, "linear", "BTC-USDT");
    REQUIRE_THROWS_AS(
        BloFinBroker("k", "s", "p", mainnet), std::runtime_error);
}

TEST_CASE("blofin broker: offline ctor accepts mainnet (guard is live-only)",
          "[blofin][broker]") {
    const auto mainnet = Resolve("blofin", Env::Mainnet, "linear", "BTC-USDT");
    REQUIRE_NOTHROW(
        BloFinBroker("k", "s", "p", mainnet, BloFinBroker::OfflineForTests{}));
}

TEST_CASE("blofin broker: MakeNonce is unique and hex-shaped",
          "[blofin][broker]") {
    const std::string a = BloFinBroker::MakeNonce();
    const std::string b = BloFinBroker::MakeNonce();
    REQUIRE(a != b);
    REQUIRE(a.find_first_not_of("0123456789abcdef-") == std::string::npos);
    REQUIRE(a.find('-') != std::string::npos);
    REQUIRE(a.find('-') == a.rfind('-'));
}

TEST_CASE("blofin broker: positive size is a long",
          "[blofin][broker]") {
    auto b = MakeOffline();
    b.HandlePrivateMessage(R"({
        "arg":{"channel":"positions"},
        "data":[{"instId":"BTC-USDT","positions":"2.5",
                 "averagePrice":"65000.0","positionSide":"net"}]
    })");
    REQUIRE(b.HasOpenPosition());
    REQUIRE(b.GetCurrentPosition().direction == SignalDirection::BUY);
    REQUIRE_THAT(b.GetCurrentPosition().volume,
                 Catch::Matchers::WithinAbs(2.5, 1e-6));
    REQUIRE_THAT(b.GetCurrentPosition().entry_price,
                 Catch::Matchers::WithinAbs(65000.0, 1e-6));
}

TEST_CASE("blofin broker: negative size is a short",
          "[blofin][broker]") {
    auto b = MakeOffline();
    b.HandlePrivateMessage(R"({
        "arg":{"channel":"positions"},
        "data":[{"instId":"BTC-USDT","positions":"-1.5",
                 "averagePrice":"65000.0","positionSide":"net"}]
    })");
    REQUIRE(b.HasOpenPosition());
    REQUIRE(b.GetCurrentPosition().direction == SignalDirection::SELL);
    REQUIRE_THAT(b.GetCurrentPosition().volume,
                 Catch::Matchers::WithinAbs(1.5, 1e-6));
}

TEST_CASE("blofin broker: zero size clears state",
          "[blofin][broker]") {
    auto b = MakeOffline();
    b.HandlePrivateMessage(R"({
        "arg":{"channel":"positions"},
        "data":[{"instId":"BTC-USDT","positions":"1.0","averagePrice":"65000",
                 "positionSide":"net"}]
    })");
    REQUIRE(b.HasOpenPosition());
    b.HandlePrivateMessage(R"({
        "arg":{"channel":"positions"},
        "data":[{"instId":"BTC-USDT","positions":"0","averagePrice":"0",
                 "positionSide":"net"}]
    })");
    REQUIRE_FALSE(b.HasOpenPosition());
    REQUIRE(b.GetCurrentPosition().direction == SignalDirection::NONE);
}

TEST_CASE("blofin broker: frame for another symbol is ignored",
          "[blofin][broker]") {
    auto b = MakeOffline();
    b.HandlePrivateMessage(R"({
        "arg":{"channel":"positions"},
        "data":[{"instId":"ETH-USDT","positions":"10","averagePrice":"3000",
                 "positionSide":"net"}]
    })");
    REQUIRE_FALSE(b.HasOpenPosition());
}

TEST_CASE("blofin broker: reduce-only filled order is booked as a trade",
          "[blofin][broker]") {
    auto b = MakeOffline();
    b.HandlePrivateMessage(R"({
        "arg":{"channel":"orders"},
        "data":[{"instId":"BTC-USDT","state":"filled","reduceOnly":"true",
                 "filledSize":"100","side":"buy","averagePrice":"65000",
                 "pnl":"12.5","updateTime":"1700000000000"}]
    })");
    REQUIRE(b.GetTradeCount() == 1);
    REQUIRE(b.GetLastTradeTime() == 1700000000000LL);
}

TEST_CASE("blofin broker: active order is not booked as a trade",
          "[blofin][broker]") {
    auto b = MakeOffline();
    b.HandlePrivateMessage(R"({
        "arg":{"channel":"orders"},
        "data":[{"instId":"BTC-USDT","state":"live","reduceOnly":"false",
                 "filledSize":"0","side":"buy","averagePrice":"0",
                 "pnl":"0","updateTime":"1700000000000"}]
    })");
    REQUIRE(b.GetTradeCount() == 0);
}

TEST_CASE("blofin broker: account equity is cached",
          "[blofin][broker]") {
    auto b = MakeOffline();
    b.HandlePrivateMessage(R"({
        "arg":{"channel":"account"},
        "data":[{"totalEquity":"25000.0"}]
    })");
    REQUIRE_THAT(b.GetBalance(),
                 Catch::Matchers::WithinAbs(25000.0, 1e-6));
    REQUIRE_THAT(b.GetEquity(),
                 Catch::Matchers::WithinAbs(25000.0, 1e-6));
}

TEST_CASE("blofin broker: raw pong is ignored",
          "[blofin][broker]") {
    auto b = MakeOffline();
    REQUIRE_NOTHROW(b.HandlePrivateMessage("pong"));
}

TEST_CASE("blofin broker: unparsable frame is dropped",
          "[blofin][broker]") {
    auto b = MakeOffline();
    REQUIRE_NOTHROW(b.HandlePrivateMessage("not json"));
    REQUIRE_FALSE(b.HasOpenPosition());
}

TEST_CASE("blofin broker: login ack with non-zero code does not throw",
          "[blofin][broker]") {
    auto b = MakeOffline();
    REQUIRE_NOTHROW(
        b.HandlePrivateMessage(R"({"event":"login","code":"60009","msg":"bad"})"));
}
