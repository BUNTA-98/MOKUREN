#include "blofin_streamer.hpp"
#include "data_integrity.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

namespace {

struct Collector {
    std::vector<L2Snapshot> books;
    std::vector<TradeEvent> trades;

    // the streamer owns an ix::WebSocket, a heartbeat std::thread, several
    // atomics and a ResyncWatchdog (which holds a std::mutex), so it is
    // neither copyable nor movable: returning it by value cannot compile.
    // hand back a unique_ptr and let the object stay where it was built.
    std::unique_ptr<BloFinDataStreamer> MakeStreamer() {
        auto s = std::make_unique<BloFinDataStreamer>("BTC-USDT", "ws://unused");
        s->SetL2Callback([this](const L2Snapshot& x) { books.push_back(x); });
        s->SetTradeCallback([this](const TradeEvent& x) { trades.push_back(x); });
        return s;
    }
};

} // namespace

TEST_CASE("blofin streamer: trade batch emits one per entry",
          "[blofin][streamer]") {
    Collector c;
    auto s = c.MakeStreamer();
    s->FeedForTesting(R"({
        "arg":{"channel":"trades","instId":"BTC-USDT"},
        "data":[
            {"instId":"BTC-USDT","price":"100","size":"1","side":"buy","ts":"1700000000000"},
            {"instId":"BTC-USDT","price":"99","size":"2","side":"sell","ts":"1700000000001"}
        ]
    })");
    REQUIRE(c.trades.size() == 2);
    REQUIRE_FALSE(c.trades[0].is_buyer_maker);   // taker=buy  -> maker=sell
    REQUIRE(c.trades[1].is_buyer_maker);         // taker=sell -> maker=buy
    // the wire `size` is in CONTRACTS; the streamer converts to base coin at
    // the producer so the tape and the book share one unit with bybit. sizes
    // 1 and 2 are therefore 0.001 and 0.002 BTC, and a tape left in contracts
    // beside a converted book would corrupt cumulative delta by 1000x while
    // every structural check still passed. compared against size *
    // contract_to_base_ -- the expression ToBase() evaluates -- so this is
    // exact, not approximate.
    REQUIRE(c.trades[0].quantity == 1 * 0.001);
    REQUIRE(c.trades[1].quantity == 2 * 0.001);
}

TEST_CASE("blofin streamer: snapshot then matching delta applies both",
          "[blofin][streamer]") {
    Collector c;
    auto s = c.MakeStreamer();
    s->FeedForTesting(R"({
        "arg":{"channel":"books","instId":"BTC-USDT"},
        "action":"snapshot",
        "data":{"bids":[[100,1]],"asks":[[101,1]],"ts":"1700000000000",
                "prevSeqId":"0","seqId":"100"}
    })");
    s->FeedForTesting(R"({
        "arg":{"channel":"books","instId":"BTC-USDT"},
        "action":"update",
        "data":{"bids":[[100,2]],"asks":[[101,3]],"ts":"1700000000100",
                "prevSeqId":"100","seqId":"150"}
    })");
    REQUIRE(c.books.size() == 2);
    REQUIRE_FALSE(mokuren::integrity::IsBookGap(c.books[0]));
    REQUIRE_FALSE(mokuren::integrity::IsBookGap(c.books[1]));
    // 2 and 3 are CONTRACTS on the wire. btc-usdt is 0.001 BTC per contract, so
    // the snapshot carries 0.002 / 0.003 base coin -- the same unit as the
    // tape, and the unit SpoofPullRule's min_qty is written in. compared
    // against the expression ToBase() evaluates: 3 * 0.001 is exact, and the
    // literal 0.007/0.003 need not be the double that product produced.
    REQUIRE(c.books[1].best_bid_qty == 2 * 0.001);
    REQUIRE(c.books[1].best_ask_qty == 3 * 0.001);
}

TEST_CASE("blofin streamer: mismatched prevSeqId emits a gap",
          "[blofin][streamer]") {
    Collector c;
    auto s = c.MakeStreamer();
    s->FeedForTesting(R"({
        "arg":{"channel":"books","instId":"BTC-USDT"},
        "action":"snapshot",
        "data":{"bids":[[100,1]],"asks":[[101,1]],"ts":"1",
                "prevSeqId":"0","seqId":"100"}
    })");
    s->FeedForTesting(R"({
        "arg":{"channel":"books","instId":"BTC-USDT"},
        "action":"update",
        "data":{"bids":[[100,2]],"asks":[[101,1]],"ts":"2",
                "prevSeqId":"999","seqId":"200"}
    })");
    REQUIRE(c.books.size() == 2);
    REQUIRE_FALSE(mokuren::integrity::IsBookGap(c.books[0]));
    REQUIRE(mokuren::integrity::IsBookGap(c.books[1]));
}

TEST_CASE("blofin streamer: a fresh snapshot after a gap re-arms sequencing",
          "[blofin][streamer]") {
    Collector c;
    auto s = c.MakeStreamer();
    s->FeedForTesting(R"({
        "arg":{"channel":"books","instId":"BTC-USDT"},
        "action":"snapshot",
        "data":{"bids":[[100,1]],"asks":[[101,1]],"ts":"1",
                "prevSeqId":"0","seqId":"100"}
    })");
    s->FeedForTesting(R"({
        "arg":{"channel":"books","instId":"BTC-USDT"},
        "action":"update",
        "data":{"bids":[[100,2]],"asks":[[101,1]],"ts":"2",
                "prevSeqId":"999","seqId":"200"}
    })");
    s->FeedForTesting(R"({
        "arg":{"channel":"books","instId":"BTC-USDT"},
        "action":"snapshot",
        "data":{"bids":[[100,5]],"asks":[[101,6]],"ts":"3",
                "prevSeqId":"0","seqId":"500"}
    })");
    const size_t before = c.books.size();
    s->FeedForTesting(R"({
        "arg":{"channel":"books","instId":"BTC-USDT"},
        "action":"update",
        "data":{"bids":[[100,7]],"asks":[[101,8]],"ts":"4",
                "prevSeqId":"500","seqId":"999"}
    })");
    REQUIRE(c.books.size() == before + 1);
    REQUIRE_FALSE(mokuren::integrity::IsBookGap(c.books.back()));
    // 7 contracts -> 0.007 BTC. 7 * 0.001 is 0.007000000000000001, so the
    // decimal literal would fail here even though the conversion is right.
    REQUIRE(c.books.back().best_bid_qty == 7 * 0.001);
}

TEST_CASE("blofin streamer: raw pong is a no-op",
          "[blofin][streamer]") {
    Collector c;
    auto s = c.MakeStreamer();
    s->FeedForTesting("pong");
    REQUIRE(c.books.empty());
    REQUIRE(c.trades.empty());
}

TEST_CASE("blofin streamer: subscribe ack is a no-op",
          "[blofin][streamer]") {
    Collector c;
    auto s = c.MakeStreamer();
    s->FeedForTesting(
        R"({"event":"subscribe","arg":{"channel":"books","instId":"BTC-USDT"}})");
    REQUIRE(c.books.empty());
    REQUIRE(c.trades.empty());
}

TEST_CASE("blofin streamer: error ack does not throw",
          "[blofin][streamer]") {
    Collector c;
    auto s = c.MakeStreamer();
    REQUIRE_NOTHROW(s->FeedForTesting(
        R"({"event":"error","code":"60012","msg":"Invalid request"})"));
}
