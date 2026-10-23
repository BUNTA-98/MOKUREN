#include "aggregator.hpp"
#include "trade_event.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

L2Snapshot Book(int64_t ts, double bid, double ask) {
  L2Snapshot snap{};
  snap.timestamp = ts;
  snap.best_bid_price = bid;
  snap.best_ask_price = ask;
  snap.bids[0].price = bid;
  snap.bids[0].qty = 1.0;
  snap.asks[0].price = ask;
  snap.asks[0].qty = 1.0;
  return snap;
}

TradeEvent Print(int64_t ts, double price) {
  TradeEvent trade{};
  trade.timestamp = ts;
  trade.price = price;
  trade.quantity = 1.0;
  trade.is_buyer_maker = false;
  return trade;
}

}  // namespace

// AttachLatestL2 skips the copy when the book has not moved. The risk in that
// optimisation is not that it copies too little for one print -- it is that it
// latches onto an old snapshot and the trigger layer keeps reading a stale
// book. These assertions are about the value the bar actually carries, not
// about the counter: the counter is an implementation detail, the frame on the
// bar is what the strategy sees.
TEST_CASE("bar carries the newest book across prints", "[aggregator]") {
  Aggregator aggregator(60'000, 0.5);
  Bar bar;

  aggregator.ProcessL2(Book(1'000, 100.0, 101.0));
  aggregator.ProcessTrade(bar, Print(60'000, 100.5));
  CHECK(bar.last_l2.timestamp == 1'000);
  CHECK(bar.last_l2.best_bid_price == 100.0);

  // second print, no book frame in between: the copy is skipped, so this is
  // the assertion that the skip did not leave an older frame on the bar.
  aggregator.ProcessTrade(bar, Print(60'001, 100.6));
  CHECK(bar.last_l2.timestamp == 1'000);
  CHECK(bar.last_l2.best_bid_price == 100.0);

  // a newer frame must reach the bar on the next print, not on the one after.
  aggregator.ProcessL2(Book(2'000, 99.5, 100.5));
  aggregator.ProcessTrade(bar, Print(60'002, 100.4));
  CHECK(bar.last_l2.timestamp == 2'000);
  CHECK(bar.last_l2.best_bid_price == 99.5);
  CHECK(bar.last_l2.best_ask_price == 100.5);
}

TEST_CASE("candle roll keeps the book attached to the bar", "[aggregator]") {
  Aggregator aggregator(60'000, 0.5);
  Bar bar;

  aggregator.ProcessL2(Book(1'000, 100.0, 101.0));
  aggregator.ProcessTrade(bar, Print(60'000, 100.5));
  REQUIRE(bar.last_l2.timestamp == 1'000);

  // past the first interval boundary: ProcessTrade closes the candle, resets
  // the bar and starts a new one. last_l2 survives the reset by design, and
  // the re-attach that used to sit here was removed -- this is what guards
  // against the bar coming out of the roll with a zeroed book.
  aggregator.ProcessTrade(bar, Print(180'000, 100.7));
  CHECK(bar.last_l2.timestamp == 1'000);
  CHECK(bar.last_l2.best_bid_price == 100.0);

  // and a book frame after the roll still lands on the new bar.
  aggregator.ProcessL2(Book(181'000, 100.2, 100.8));
  aggregator.ProcessTrade(bar, Print(180'001, 100.6));
  CHECK(bar.last_l2.timestamp == 181'000);
  CHECK(bar.last_l2.best_bid_price == 100.2);
}
