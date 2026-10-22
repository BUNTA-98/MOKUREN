// Unit tests for the local book that turns a snapshot+delta feed into the
// complete L2Snapshots the validator and the depth triggers expect.
//
// This is the only stateful logic between a socket and the engine, and it used
// to be unreachable without a live connection. A book that is quietly wrong
// still produces structurally valid frames -- every check in data_integrity is
// self-consistency, not correctness -- so these cases are the only thing
// between a missed delta and a confident wrong signal.

#include "order_book.hpp"

#include <catch2/catch_test_macros.hpp>

#include <utility>
#include <vector>

using mokuren::OrderBook;

namespace {

using Level = OrderBook::Level;

std::vector<Level> Levels(std::initializer_list<std::pair<double, double>> xs) {
  std::vector<Level> out;
  out.reserve(xs.size());
  for (const auto& x : xs) out.emplace_back(x.first, x.second);
  return out;
}

// bids 100/99, asks 101/102, numbered from update id 10.
OrderBook TwoSidedBook() {
  OrderBook book;
  book.ApplySnapshot(Levels({{100.0, 1.0}, {99.0, 2.0}}),
                     Levels({{101.0, 1.0}, {102.0, 2.0}}), /*update_id=*/10);
  return book;
}

}  // namespace

TEST_CASE("a snapshot lays the book out the way the validator reads it") {
  const OrderBook book = TwoSidedBook();
  const L2Snapshot snap = book.ToSnapshot(1234);

  CHECK(snap.timestamp == 1234);
  CHECK(snap.bids[0].price == 100.0);
  CHECK(snap.bids[0].qty == 1.0);
  CHECK(snap.bids[1].price == 99.0);
  CHECK(snap.asks[0].price == 101.0);
  CHECK(snap.asks[1].price == 102.0);

  // the legacy fields are derived, not carried: a mismatch between them and
  // the arrays is how a gap sentinel gets misread (IsBookGap reads these).
  CHECK(snap.best_bid_price == 100.0);
  CHECK(snap.best_bid_qty == 1.0);
  CHECK(snap.best_ask_price == 101.0);
  CHECK(snap.best_ask_qty == 1.0);

  // unused slots stay zeroed: that is the end-of-side marker everything else
  // depends on.
  CHECK(snap.bids[2].price == 0.0);
  CHECK(snap.asks[2].price == 0.0);
}

TEST_CASE("side order is imposed, not trusted from the wire") {
  OrderBook book;
  book.ApplySnapshot(Levels({{99.0, 2.0}, {100.0, 1.0}, {98.0, 3.0}}),
                     Levels({{102.0, 2.0}, {101.0, 1.0}}), 1);

  const L2Snapshot snap = book.ToSnapshot(1);
  CHECK(snap.bids[0].price == 100.0);
  CHECK(snap.bids[1].price == 99.0);
  CHECK(snap.bids[2].price == 98.0);
  CHECK(snap.asks[0].price == 101.0);
  CHECK(snap.asks[1].price == 102.0);
}

TEST_CASE("a delta with size zero removes the level") {
  OrderBook book = TwoSidedBook();

  CHECK(book.ApplyDelta(Levels({{99.0, 0.0}}), {}, /*update_id=*/11) ==
        OrderBook::ApplyResult::Applied);
  CHECK(book.bid_depth() == 1);

  const L2Snapshot snap = book.ToSnapshot(1);
  CHECK(snap.bids[0].price == 100.0);
  CHECK(snap.bids[1].price == 0.0);
}

TEST_CASE("a delta inserts a new level in sorted position") {
  OrderBook book = TwoSidedBook();

  CHECK(book.ApplyDelta(Levels({{100.5, 5.0}}), Levels({{100.9, 7.0}}),
                        /*update_id=*/11) == OrderBook::ApplyResult::Applied);

  const L2Snapshot snap = book.ToSnapshot(1);
  CHECK(snap.bids[0].price == 100.5);
  CHECK(snap.bids[1].price == 100.0);
  CHECK(snap.asks[0].price == 100.9);
  CHECK(snap.asks[1].price == 101.0);
}

TEST_CASE("a delta before any snapshot is ignored") {
  OrderBook book;

  CHECK(book.ApplyDelta(Levels({{100.0, 1.0}}), Levels({{101.0, 1.0}}), 1) ==
        OrderBook::ApplyResult::Ignored);
  CHECK(book.empty());
  CHECK_FALSE(book.has_snapshot());
}

TEST_CASE("a skipped update id is a gap, and the book is left untouched") {
  OrderBook book = TwoSidedBook();  // last id 10

  CHECK(book.ApplyDelta(Levels({{100.0, 9.0}}), {}, /*update_id=*/12) ==
        OrderBook::ApplyResult::GapDetected);

  // untouched: the caller decides whether to resync, and a half-applied frame
  // would make the visible book disagree with the sequence cursor.
  const L2Snapshot snap = book.ToSnapshot(1);
  CHECK(snap.bids[0].qty == 1.0);
  CHECK(book.last_update_id() == 10);
}

TEST_CASE("an unnumbered venue never reports a gap") {
  OrderBook book;
  book.ApplySnapshot(Levels({{100.0, 1.0}}), Levels({{101.0, 1.0}}),
                     /*update_id=*/0);

  CHECK(book.ApplyDelta(Levels({{100.0, 2.0}}), {}, /*update_id=*/0) ==
        OrderBook::ApplyResult::Applied);
  CHECK(book.ApplyDelta(Levels({{100.0, 3.0}}), {}, /*update_id=*/0) ==
        OrderBook::ApplyResult::Applied);
  CHECK(book.last_update_id() == 0);
}

TEST_CASE("Clear drops the cursor so a stale delta after a reconnect is ignored") {
  OrderBook book = TwoSidedBook();
  book.Clear();

  CHECK(book.empty());
  CHECK_FALSE(book.has_snapshot());
  CHECK(book.last_update_id() == 0);

  // the same id the old connection had reached: applying it would resurrect a
  // book built by a connection that no longer exists.
  CHECK(book.ApplyDelta(Levels({{100.0, 1.0}}), {}, /*update_id=*/11) ==
        OrderBook::ApplyResult::Ignored);
}

TEST_CASE("a snapshot after a gap restores the book and the cursor") {
  OrderBook book = TwoSidedBook();
  REQUIRE(book.ApplyDelta({}, {}, 12) == OrderBook::ApplyResult::GapDetected);

  book.Clear();
  CHECK(book.ApplySnapshot(Levels({{50.0, 1.0}}), Levels({{51.0, 1.0}}),
                           /*update_id=*/99) ==
        OrderBook::ApplyResult::Applied);

  CHECK(book.last_update_id() == 99);
  CHECK(book.ApplyDelta(Levels({{50.0, 2.0}}), {}, /*update_id=*/100) ==
        OrderBook::ApplyResult::Applied);
}

TEST_CASE("the published frame is truncated at MAX_L2_DEPTH") {
  // each side is generated in the direction it will be published. the previous
  // version built the asks from the same descending expression as the bids, so
  // sorting them ascending kept the *worst* 20 of 25 and the deepest slot was
  // five levels in from where the assertion expected it. that was a bug in the
  // test, not in OrderBook: 25 asks generated downward start at 976.5, and the
  // 20th kept is 995.5.
  const size_t extra = 5;
  std::vector<Level> bids;
  std::vector<Level> asks;
  for (size_t i = 0; i < static_cast<size_t>(MAX_L2_DEPTH) + extra; ++i) {
    bids.emplace_back(1000.0 - static_cast<double>(i), 1.0);  // descending
    asks.emplace_back(1001.0 + static_cast<double>(i), 1.0);  // ascending
  }

  OrderBook book;
  book.ApplySnapshot(bids, asks, 1);
  CHECK(book.bid_depth() == static_cast<size_t>(MAX_L2_DEPTH) + extra);
  CHECK(book.ask_depth() == static_cast<size_t>(MAX_L2_DEPTH) + extra);

  const L2Snapshot snap = book.ToSnapshot(1);
  const size_t last = static_cast<size_t>(MAX_L2_DEPTH) - 1;

  // truncation keeps the best levels, not the first ones the venue happened to
  // list: the top of each side is unchanged by it.
  CHECK(snap.bids[0].price == 1000.0);
  CHECK(snap.asks[0].price == 1001.0);

  // and the deepest slot the engine can hold is the MAX_L2_DEPTH-th best, not
  // the last one the venue sent.
  CHECK(snap.bids[last].price == 1000.0 - static_cast<double>(last));
  CHECK(snap.asks[last].price == 1001.0 + static_cast<double>(last));
}

TEST_CASE("a non-positive price is skipped rather than stored") {
  OrderBook book;
  book.ApplySnapshot(Levels({{0.0, 1.0}, {-5.0, 1.0}, {100.0, 1.0}}),
                     Levels({{101.0, 1.0}}), 1);

  CHECK(book.bid_depth() == 1);
  const L2Snapshot snap = book.ToSnapshot(1);
  CHECK(snap.bids[0].price == 100.0);
}
