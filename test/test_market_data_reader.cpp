#include "market_data/format.hpp"
#include "market_data/merger.hpp"
#include "market_data/reader.hpp"
#include "market_data/writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// The writer patches the file header in Close(), and the destructor calls it,
// so a case that fails mid-way still cleans up after itself here.
struct ScopedFile {
  std::string path;

  ~ScopedFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};

std::string TempPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

mokuren::mdf::SessionInfo BtcSession() {
  mokuren::mdf::SessionInfo info;
  info.venue = "bybit";
  info.env = "mainnet";
  info.category = "linear";
  info.symbol = "BTCUSDT";
  info.source = "live";
  return info;
}

mokuren::mdf::BookLevel Level(double price, double qty) {
  return mokuren::mdf::BookLevel{price, qty};
}

mokuren::mdf::BookChange Change(uint8_t side, uint8_t action, double price,
                                double qty) {
  mokuren::mdf::BookChange change{};
  change.side = side;
  change.action = action;
  change.price = price;
  change.qty = qty;
  return change;
}

}  // namespace

TEST_CASE("mdf reader round-trips a snapshot-only recording", "[mdf]") {
  ScopedFile file{TempPath("mokuren_mdf_roundtrip.mkn")};

  const int64_t trade_ns = 1'700'000'000'123LL * 1'000'000LL;
  {
    mokuren::mdf::Writer writer;
    REQUIRE(writer.Open(file.path, BtcSession()));

    mokuren::mdf::TradeRec trade{};
    trade.ts_exchange_ns = trade_ns;
    trade.ts_recv_ns = trade_ns;
    trade.price = 50000.5;
    trade.qty = 0.25;
    // aggressor Sell == the resting bid was hit == maker was the buyer.
    trade.aggressor = static_cast<uint8_t>(mokuren::mdf::Aggressor::Sell);
    REQUIRE(writer.WriteTrade(trade));

    const mokuren::mdf::BookLevel levels[] = {
        Level(50000.0, 1.0), Level(49999.0, 2.0),
        Level(50001.0, 1.5), Level(50002.0, 0.5)};
    mokuren::mdf::BookSnapshotRec snapshot{};
    snapshot.ts_exchange_ns = trade_ns;
    snapshot.ts_recv_ns = trade_ns;
    snapshot.seq = 7;
    snapshot.n_bids = 2;
    snapshot.n_asks = 2;
    REQUIRE(writer.WriteSnapshot(snapshot, levels, 4));

    REQUIRE(writer.Close());
  }

  const mokuren::mdf::SessionData session =
      mokuren::mdf::ReadSessionFile(file.path);

  REQUIRE(session.ok());
  CHECK(session.warnings.empty());
  REQUIRE(session.meta.has_metadata);
  CHECK(session.meta.venue == "bybit");
  CHECK(session.meta.symbol == "BTCUSDT");
  CHECK(session.meta.depth_per_side == mokuren::mdf::kMaxBookDepth);

  REQUIRE(session.trades.size() == 1);
  CHECK(session.trades[0].timestamp == 1'700'000'000'123LL);
  CHECK(session.trades[0].price == 50000.5);
  CHECK(session.trades[0].quantity == 0.25);
  CHECK(session.trades[0].is_buyer_maker);

  REQUIRE(session.l2.size() == 1);
  CHECK(session.l2[0].timestamp == 1'700'000'000'123LL);
  CHECK(session.l2[0].best_bid_price == 50000.0);
  CHECK(session.l2[0].best_bid_qty == 1.0);
  CHECK(session.l2[0].best_ask_price == 50001.0);
  CHECK(session.l2[0].bids[1].price == 49999.0);
  CHECK(session.l2[0].bids[1].qty == 2.0);
  CHECK(session.l2[0].asks[1].price == 50002.0);
  // Unused slots stay zeroed: the depth triggers read them.
  CHECK(session.l2[0].bids[2].price == 0.0);
}

TEST_CASE("mdf reader recovers blocks from an unfinalised recording", "[mdf]") {
  ScopedFile file{TempPath("mokuren_mdf_truncated.mkn")};

  uint64_t metadata_offset = 0;
  {
    mokuren::mdf::Writer writer;
    REQUIRE(writer.Open(file.path, BtcSession()));

    mokuren::mdf::TradeRec trade{};
    trade.ts_exchange_ns = 1'700'000'000'000LL * 1'000'000LL;
    trade.price = 1.0;
    trade.qty = 1.0;
    trade.aggressor = static_cast<uint8_t>(mokuren::mdf::Aggressor::Buy);
    REQUIRE(writer.WriteTrade(trade));
    REQUIRE(writer.Close());

    std::ifstream in(file.path, std::ios::binary);
    mokuren::mdf::FileHeader header{};
    REQUIRE(in.read(reinterpret_cast<char*>(&header), sizeof(header)));
    metadata_offset = header.metadata_offset;
    REQUIRE(metadata_offset > sizeof(mokuren::mdf::FileHeader));
  }

  // A SIGKILL before Close() leaves intact blocks and no trailer. Cutting the
  // file at the metadata start reproduces exactly that byte layout.
  std::filesystem::resize_file(file.path, metadata_offset);

  const mokuren::mdf::SessionData session =
      mokuren::mdf::ReadSessionFile(file.path);

  CHECK(session.ok());
  REQUIRE(session.trades.size() == 1);
  CHECK(session.trades[0].timestamp == 1'700'000'000'000LL);
  CHECK_FALSE(session.meta.has_metadata);
  CHECK_FALSE(session.warnings.empty());
}

TEST_CASE("mdf reader skips a block whose payload was corrupted", "[mdf]") {
  ScopedFile file{TempPath("mokuren_mdf_corrupt.mkn")};

  {
    mokuren::mdf::Writer writer;
    REQUIRE(writer.Open(file.path, BtcSession()));

    mokuren::mdf::TradeRec trade{};
    trade.ts_exchange_ns = 1'700'000'000'000LL * 1'000'000LL;
    trade.price = 42.0;
    trade.qty = 1.0;
    trade.aggressor = static_cast<uint8_t>(mokuren::mdf::Aggressor::Buy);
    REQUIRE(writer.WriteTrade(trade));

    const mokuren::mdf::BookLevel levels[] = {Level(100.0, 1.0),
                                              Level(101.0, 1.0)};
    mokuren::mdf::BookSnapshotRec snapshot{};
    snapshot.ts_exchange_ns = trade.ts_exchange_ns;
    snapshot.seq = 1;
    snapshot.n_bids = 1;
    snapshot.n_asks = 1;
    REQUIRE(writer.WriteSnapshot(snapshot, levels, 2));

    REQUIRE(writer.Close());
  }

  // FlushAll writes the trade block first, so the first payload starts right
  // after the header and the block header. Flip one byte of it.
  {
    std::fstream out(file.path, std::ios::binary | std::ios::in |
                                     std::ios::out);
    REQUIRE(out);
    const std::streamoff byte = static_cast<std::streamoff>(
        sizeof(mokuren::mdf::FileHeader) + mokuren::mdf::kBlockHeaderSize + 3);
    out.seekg(byte);
    char value = 0;
    REQUIRE(out.read(&value, 1));
    value = static_cast<char>(value ^ 0x5A);
    out.seekp(byte);
    REQUIRE(out.write(&value, 1));
  }

  const mokuren::mdf::SessionData session =
      mokuren::mdf::ReadSessionFile(file.path);

  CHECK(session.trades.empty());
  REQUIRE(session.l2.size() == 1);
  CHECK(session.l2[0].best_bid_price == 100.0);
  REQUIRE_FALSE(session.warnings.empty());
}


TEST_CASE("mdf reader replays book deltas onto the last snapshot", "[mdf]") {
  ScopedFile file{TempPath("mokuren_mdf_delta.mkn")};

  const int64_t base_ns = 1'700'000'000'000LL * 1'000'000LL;
  {
    mokuren::mdf::Writer writer;
    REQUIRE(writer.Open(file.path, BtcSession()));

    const mokuren::mdf::BookLevel levels[] = {
        Level(100.0, 1.0), Level(99.0, 1.0),
        Level(101.0, 1.0), Level(102.0, 1.0)};
    mokuren::mdf::BookSnapshotRec snapshot{};
    snapshot.ts_exchange_ns = base_ns;
    snapshot.ts_recv_ns = base_ns;
    snapshot.seq = 1;
    snapshot.n_bids = 2;
    snapshot.n_asks = 2;
    REQUIRE(writer.WriteSnapshot(snapshot, levels, 4));

    const std::vector<mokuren::mdf::BookChange> changes = {
        Change(mokuren::mdf::book_change_side::kBid,
               mokuren::mdf::book_change_action::kSet, 100.0, 2.5),
        Change(mokuren::mdf::book_change_side::kAsk,
               mokuren::mdf::book_change_action::kRemove, 101.0, 0.0),
    };
    mokuren::mdf::BookDeltaRec delta{};
    delta.ts_exchange_ns = base_ns + 1'000'000LL;  // +1 ms
    delta.ts_recv_ns = delta.ts_exchange_ns;
    delta.seq = 2;
    delta.n_changes = static_cast<uint16_t>(changes.size());
    REQUIRE(writer.WriteDelta(delta, changes.data()));

    REQUIRE(writer.Close());
  }

  const mokuren::mdf::SessionData session =
      mokuren::mdf::ReadSessionFile(file.path);

  REQUIRE(session.ok());
  REQUIRE(session.l2.size() == 2);

  // The snapshot is emitted as written; the delta is applied to it.
  CHECK(session.l2[1].timestamp == 1'700'000'000'001LL);
  CHECK(session.l2[1].bids[0].price == 100.0);
  CHECK(session.l2[1].bids[0].qty == 2.5);
  CHECK(session.l2[1].bids[1].price == 99.0);
  CHECK(session.l2[1].asks[0].price == 102.0);  // 101.0 was removed
  CHECK(session.l2[1].asks[1].price == 0.0);
}

TEST_CASE("mdf merger rejects an invalid window key", "[mdf][merger]") {
  // Weekly: a negative key is not a date this codebase can produce, and
  // DayStringFromKey would format it as a plausible pre-epoch date rather than
  // reject it. The empty string is the contract MergeWindow checks.
  CHECK(mokuren::mdf::WindowLabelFromKey(
            -1, mokuren::mdf::WindowKind::Weekly)
            .empty());

  // Monthly: out-of-range year/month must not produce a clamped or negative
  // label -- a wrong-but-plausible month names an output file after data it
  // does not contain.
  CHECK(mokuren::mdf::WindowLabelFromKey(
            202413, mokuren::mdf::WindowKind::Monthly)
            .empty());
  CHECK(mokuren::mdf::WindowLabelFromKey(
            196912, mokuren::mdf::WindowKind::Monthly)
            .empty());

  // A valid key still formats, so the guard above is not rejecting everything.
  CHECK(mokuren::mdf::WindowLabelFromKey(
            202401, mokuren::mdf::WindowKind::Monthly) == "2024-01");
}
