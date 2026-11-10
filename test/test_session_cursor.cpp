// Equivalence tests for SessionCursor against the materialising reader.
//
// These exist because the weekly backtest runs on the cursor, not on
// ReadSessionFile: nothing else in the suite exercises the reorder window, and
// a window that is too small is invisible from the outside -- the stream stays
// ascending-looking until the first late print, at which point SliceTrades
// stops at the wrong record and the scan reports a shorter dataset than it was
// given.
//
// The fixtures are synthetic recordings written through the real Writer, in
// deliberately unhelpful orders. That is the point: a test that writes blocks
// already in timestamp order proves nothing about a reorder buffer.

#include "data_manager.hpp"
#include "market_data/reader.hpp"
#include "market_data/writer.hpp"
#include "trade_event.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// A recording that deletes itself. The rest of this suite is pure functions of
// structs, so there is no shared temp-dir helper to borrow; the fixture owns its
// path and removes it in the destructor, so a failing REQUIRE leaves nothing for
// the next run to open.
class TempRecording {
 public:
  TempRecording() {
    static std::atomic<unsigned> counter{0};
    const auto tag = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = fs::temp_directory_path() /
            ("mokuren_cursor_test_" + std::to_string(tag) + "_" +
             std::to_string(counter.fetch_add(1)) + ".mkn");
  }
  ~TempRecording() {
    std::error_code ec;
    fs::remove(path_, ec);
  }
  TempRecording(const TempRecording&) = delete;
  TempRecording& operator=(const TempRecording&) = delete;

  std::string path() const { return path_.string(); }

 private:
  fs::path path_;
};

mokuren::mdf::SessionInfo Info() {
  mokuren::mdf::SessionInfo i;
  i.venue = "test";
  i.env = "test";
  i.category = "linear";
  i.symbol = "TESTUSDT";
  i.source = "replay";
  return i;
}

// One record per block, flushed immediately. With the default 4096/1s the
// writer coalesces the fixture into a single block and the recorded ORDER OF
// BLOCKS -- the thing under test -- is lost.
mokuren::mdf::Writer::Options OnePerBlock() {
  mokuren::mdf::Writer::Options o;
  o.max_records_per_block = 1;
  o.max_block_span_ns = 1;
  o.max_levels_per_side = 2;
  return o;
}

// Two records per block, spanning the default 1 s. Used by the range case,
// where a single-record block cannot straddle a window edge.
mokuren::mdf::Writer::Options TwoPerBlock() {
  mokuren::mdf::Writer::Options o;
  o.max_records_per_block = 2;
  o.max_block_span_ns = 1'000'000'000;
  o.max_levels_per_side = 2;
  return o;
}

constexpr int64_t Ms(int64_t ms) { return ms * 1'000'000LL; }

mokuren::mdf::TradeRec TradeRecAt(int64_t ts_ms, double price) {
  mokuren::mdf::TradeRec r{};
  r.ts_exchange_ns = Ms(ts_ms);
  r.ts_recv_ns     = Ms(ts_ms);
  r.price          = price;
  r.qty            = 1.0;
  r.aggressor      = static_cast<uint8_t>(mokuren::mdf::Aggressor::Buy);
  return r;
}

// A structurally valid delta-1 book: one bid, one ask, both sides populated.
// A one-sided frame is rejected by DecodeSnapshotsImpl, which would make the
// fixture disappear from both readers rather than exercise the cursor.
mokuren::mdf::BookSnapshotRec BookRecAt(int64_t ts_ms, int64_t seq) {
  mokuren::mdf::BookSnapshotRec rec{};
  rec.ts_exchange_ns = Ms(ts_ms);
  rec.ts_recv_ns     = Ms(ts_ms);
  rec.seq            = seq;
  rec.n_bids         = 1;
  rec.n_asks         = 1;
  return rec;
}

void FillBook(mokuren::mdf::BookLevel* levels, double bid, double ask) {
  // bids best-first, then asks best-first; unused stride slots stay zero.
  levels[0] = mokuren::mdf::BookLevel{bid, 1.0};
  levels[1] = mokuren::mdf::BookLevel{ask, 1.0};
}

}  // namespace

// ---------------------------------------------------------------------------
// ordering
// ---------------------------------------------------------------------------

TEST_CASE("the cursor reproduces the materialising reader on a recording "
          "whose blocks are not in timestamp order") {
  TempRecording tmp;

  // Written in a consciously shuffled order. The recorder produces this shape
  // by itself: Writer keeps a late print rather than dropping it, which is
  // exactly what Meta::max_inversion_ns measures. A reader that emits in block
  // order hands SliceTrades an interleaved stream, and SliceTrades stops at the
  // first record past end_time -- so the run silently loses its tail.
  const int64_t written_order_ms[] = {2000, 1000, 3000, 1500, 2500};
  {
    mokuren::mdf::Writer w;
    REQUIRE(w.Open(tmp.path(), Info(), OnePerBlock()));
    for (int64_t ms : written_order_ms) {
      REQUIRE(w.WriteTrade(TradeRecAt(ms, 100.0 + ms / 1000.0)));
    }
    REQUIRE(w.Close());
  }

  const auto full = mokuren::mdf::ReadSessionFile(tmp.path());
  REQUIRE(full.ok());
  REQUIRE(full.trades.size() == 5);

  mokuren::mdf::SessionCursor cursor;
  REQUIRE(cursor.Open(tmp.path(), mokuren::mdf::Channel::Trades));

  std::vector<TradeEvent> streamed;
  while (cursor.HasNext()) {
    streamed.push_back(cursor.FrontTrade());
    cursor.Advance();
  }
  REQUIRE(cursor.status().ok());

  REQUIRE(streamed.size() == full.trades.size());
  for (size_t i = 0; i < streamed.size(); ++i) {
    CHECK(streamed[i].timestamp      == full.trades[i].timestamp);
    CHECK(streamed[i].price          == full.trades[i].price);
    CHECK(streamed[i].quantity       == full.trades[i].quantity);
    CHECK(streamed[i].is_buyer_maker == full.trades[i].is_buyer_maker);
  }

  // Asserted by value as well, so a change that makes BOTH readers wrong in
  // the same way still fails here instead of agreeing with itself.
  REQUIRE(streamed[0].timestamp == 1000);
  REQUIRE(streamed[1].timestamp == 1500);
  REQUIRE(streamed[2].timestamp == 2000);
  REQUIRE(streamed[3].timestamp == 2500);
  REQUIRE(streamed[4].timestamp == 3000);
}

TEST_CASE("two prints in the same millisecond keep their arrival order") {
  TempRecording tmp;
  {
    mokuren::mdf::Writer w;
    REQUIRE(w.Open(tmp.path(), Info(), OnePerBlock()));
    // Distinct prices so either ordering is visible in the assertion.
    REQUIRE(w.WriteTrade(TradeRecAt(1000, 100.0)));
    REQUIRE(w.WriteTrade(TradeRecAt(1000, 101.0)));
    REQUIRE(w.Close());
  }

  const auto full = mokuren::mdf::ReadSessionFile(tmp.path());
  REQUIRE(full.ok());
  REQUIRE(full.trades.size() == 2);
  // stable_sort on the materialising path: ties keep arrival order.
  REQUIRE(full.trades[0].price == 100.0);
  REQUIRE(full.trades[1].price == 101.0);

  mokuren::mdf::SessionCursor cursor;
  REQUIRE(cursor.Open(tmp.path(), mokuren::mdf::Channel::Trades));

  std::vector<double> streamed;
  while (cursor.HasNext()) {
    streamed.push_back(cursor.FrontTrade().price);
    cursor.Advance();
  }
  REQUIRE(cursor.status().ok());

  REQUIRE(streamed.size() == 2);
  // arrival_seq is the tiebreak; a heap keyed on timestamp alone can emit
  // these in either order and would break clustered/footprint triggers.
  CHECK(streamed[0] == 100.0);
  CHECK(streamed[1] == 101.0);
}

// ---------------------------------------------------------------------------
// range
// ---------------------------------------------------------------------------

TEST_CASE("a ranged cursor equals the full decode filtered to the window, "
          "record for record") {
  TempRecording tmp;

  // Two blocks that each straddle a window edge. A block-atomic gate would
  // return the whole block and disagree with what the runners consume, which
  // is the record-precise filtering SliceTrades/SliceL2 already promise.
  const int64_t all_ms[] = {1000, 2000, 3000, 4000};
  {
    mokuren::mdf::Writer w;
    REQUIRE(w.Open(tmp.path(), Info(), TwoPerBlock()));
    for (int64_t ms : all_ms) {
      REQUIRE(w.WriteTrade(TradeRecAt(ms, 100.0 + ms / 1000.0)));
    }
    REQUIRE(w.Close());
  }

  const auto full = mokuren::mdf::ReadSessionFile(tmp.path());
  REQUIRE(full.ok());
  REQUIRE(full.trades.size() == 4);

  mokuren::mdf::SessionCursor cursor;
  REQUIRE(cursor.Open(tmp.path(), mokuren::mdf::Channel::Trades,
                      Ms(1500), Ms(3500)));

  std::vector<int64_t> got;
  while (cursor.HasNext()) {
    got.push_back(cursor.FrontTrade().timestamp);
    cursor.Advance();
  }
  REQUIRE(cursor.status().ok());

  REQUIRE(got.size() == 2);
  CHECK(got[0] == 2000);
  CHECK(got[1] == 3000);
}

// ---------------------------------------------------------------------------
// channel selection
// ---------------------------------------------------------------------------

TEST_CASE("a cursor refuses to stream both channels at once") {
  TempRecording tmp;
  {
    mokuren::mdf::Writer w;
    REQUIRE(w.Open(tmp.path(), Info(), OnePerBlock()));
    REQUIRE(w.WriteTrade(TradeRecAt(1000, 100.0)));
    REQUIRE(w.Close());
  }

  mokuren::mdf::SessionCursor cursor;
  CHECK_FALSE(cursor.Open(tmp.path(), mokuren::mdf::Channel::Both));
  CHECK_FALSE(cursor.status().error.empty());
}

TEST_CASE("the channel mask keeps the book out of LoadAllTrades and trades out "
          "of LoadAllL2Snapshots") {
  TempRecording tmp;
  {
    mokuren::mdf::Writer w;
    REQUIRE(w.Open(tmp.path(), Info(), OnePerBlock()));
    REQUIRE(w.WriteTrade(TradeRecAt(1000, 100.0)));

    mokuren::mdf::BookLevel levels[4]{};
    FillBook(levels, 100.0, 100.5);
    REQUIRE(w.WriteSnapshot(BookRecAt(1000, 1), levels, 2));

    REQUIRE(w.Close());
  }

  // The whole point of the mask: neither loader pays for the channel it does
  // not return. Asserting the sizes is the observable half; the memory saved
  // is the other half and is not something a unit test can see.
  const std::vector<TradeEvent> trades = DataManager::LoadAllTrades(tmp.path());
  CHECK(trades.size() == 1);

  const std::vector<L2Snapshot> book = DataManager::LoadAllL2Snapshots(tmp.path());
  CHECK(book.size() == 1);
  CHECK(book[0].best_bid_price == 100.0);
  CHECK(book[0].best_ask_price == 100.5);

  // Trades and book agree on the timestamp, so a mask that decoded the wrong
  // channel would still return one record of the wrong type -- compare a field
  // only the other type has.
  CHECK(trades[0].price == 100.0);
}

// ---------------------------------------------------------------------------
// book stream
// ---------------------------------------------------------------------------

TEST_CASE("the cursor streams book frames in the same order as the "
          "materialising reader") {
  TempRecording tmp;
  {
    mokuren::mdf::Writer w;
    REQUIRE(w.Open(tmp.path(), Info(), OnePerBlock()));

    mokuren::mdf::BookLevel levels[4]{};
    for (int64_t ms : {2000, 1000, 3000}) {
      FillBook(levels, 100.0 + ms / 1000.0, 100.5 + ms / 1000.0);
      REQUIRE(w.WriteSnapshot(BookRecAt(ms, ms), levels, 2));
    }
    REQUIRE(w.Close());
  }

  const auto full = mokuren::mdf::ReadSessionFile(tmp.path());
  REQUIRE(full.ok());
  REQUIRE(full.l2.size() == 3);

  mokuren::mdf::SessionCursor cursor;
  REQUIRE(cursor.Open(tmp.path(), mokuren::mdf::Channel::L2));

  std::vector<L2Snapshot> streamed;
  while (cursor.HasNext()) {
    streamed.push_back(cursor.FrontL2());
    cursor.Advance();
  }
  REQUIRE(cursor.status().ok());

  REQUIRE(streamed.size() == full.l2.size());
  for (size_t i = 0; i < streamed.size(); ++i) {
    CHECK(streamed[i].timestamp      == full.l2[i].timestamp);
    CHECK(streamed[i].best_bid_price == full.l2[i].best_bid_price);
    CHECK(streamed[i].best_ask_price == full.l2[i].best_ask_price);
  }
}
