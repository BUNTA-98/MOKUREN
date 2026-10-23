#include "market_data/reader.hpp"

#include "market_data/crc32c.hpp"
#include "order_book.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>

namespace mokuren::mdf {
namespace {

// A payload larger than this is a corrupt size field, not a recording. The
// guard exists so a bad uint64 cannot make the reader allocate the address
// space before noticing.
constexpr uint64_t kMaxPayloadBytes = 512ull * 1024ull * 1024ull;

int64_t NsToMs(int64_t ns) {
  // Floor, not truncation toward zero. Every timestamp a validator lets through
  // is positive, so this branch only runs on a corrupt record -- and rounding
  // that one toward zero would move it a whole millisecond closer to a
  // plausible date, which is the wrong direction for a corrupt value.
  if (ns >= 0) return ns / 1'000'000;
  return -(((-ns) + 999'999) / 1'000'000);
}

bool ReadExactAt(std::ifstream& in, uint64_t offset, void* buf, size_t n) {
  in.clear();
  in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!in) return false;
  return static_cast<bool>(
      in.read(static_cast<char*>(buf), static_cast<std::streamsize>(n)));
}

struct FileLayout {
  FileHeader header{};
  uint64_t data_end = 0;  // where the last block may end
};

bool ReadFileHeader(std::ifstream& in, uint64_t file_size, FileLayout& layout,
                    std::string& error) {
  if (file_size < sizeof(FileHeader)) {
    error = "file is smaller than the format header (" +
            std::to_string(file_size) + " bytes)";
    return false;
  }
  if (!ReadExactAt(in, 0, &layout.header, sizeof(FileHeader))) {
    error = "could not read the format header";
    return false;
  }

  const FileHeader& h = layout.header;
  if (std::memcmp(h.magic, kFileMagic, sizeof(h.magic)) != 0) {
    error = "not a .mkn file: magic mismatch";
    return false;
  }
  if (h.endian_tag != kEndianTagLe) {
    error = "unsupported byte order (endian tag " +
            std::to_string(h.endian_tag) + ")";
    return false;
  }
  if (h.double_size != sizeof(double)) {
    error = "writer used " + std::to_string(h.double_size) +
            "-byte doubles, this reader uses " + std::to_string(sizeof(double));
    return false;
  }
  if (h.min_reader_version > kFormatVersion) {
    error = "file requires reader version " +
            std::to_string(h.min_reader_version) + ", this build implements " +
            std::to_string(kFormatVersion);
    return false;
  }
  if (h.header_size < kFileHeaderSize || h.header_size > file_size) {
    error = "header_size field is out of range (" +
            std::to_string(h.header_size) + ")";
    return false;
  }

  // Blocks end where the metadata begins. An unfinalised recording (crash,
  // SIGKILL) has no metadata and no index, so every offset in the header is
  // still zero: fall back to the actual end of the file and keep everything
  // written before the damage.
  if (h.metadata_offset >= h.header_size && h.metadata_offset < file_size) {
    layout.data_end = h.metadata_offset;
  } else if (h.file_size >= h.header_size && h.file_size <= file_size) {
    layout.data_end = h.file_size;
  } else {
    layout.data_end = file_size;
  }
  return true;
}

void ParseMetadata(std::ifstream& in, const FileLayout& layout,
                   uint64_t file_size, SessionData& out) {
  const FileHeader& h = layout.header;
  if (h.metadata_size == 0) return;

  if (h.metadata_offset < h.header_size ||
      h.metadata_size > kMaxPayloadBytes ||
      h.metadata_offset + h.metadata_size > file_size) {
    out.warnings.push_back(
        "metadata block lies outside the file; session fields unknown");
    return;
  }

  std::string bytes(static_cast<size_t>(h.metadata_size), '\0');
  if (!ReadExactAt(in, h.metadata_offset, bytes.data(), bytes.size())) {
    out.warnings.push_back("could not read the metadata block");
    return;
  }
  if (Crc32c(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()) !=
      h.metadata_crc32c) {
    out.warnings.push_back(
        "metadata checksum mismatch; session fields may be wrong");
  }

  nlohmann::json json;
  try {
    json = nlohmann::json::parse(bytes);
  } catch (const std::exception& e) {
    out.warnings.push_back(std::string("metadata is not valid json: ") +
                           e.what());
    return;
  }
  if (!json.is_object()) {
    out.warnings.push_back("metadata is not a json object");
    return;
  }

  out.meta.has_metadata = true;
  try {
    out.meta.venue = json.value("venue", std::string{});
    out.meta.env = json.value("env", std::string{});
    out.meta.category = json.value("category", std::string{});
    out.meta.symbol = json.value("symbol", std::string{});
    out.meta.source = json.value("source", std::string{});
    out.meta.depth_per_side = json.value("depth_per_side", uint32_t{0});
    out.meta.created_unix_ns = json.value("created_unix_ns", int64_t{0});
  } catch (const std::exception& e) {
    // A present-but-wrong-typed field is metadata damage, not a reason to
    // refuse the blocks: the records are what the backtest runs on.
    out.warnings.push_back(std::string("metadata field has an unexpected "
                                       "type: ") + e.what());
  }
}

void DecodeTrades(const BlockHeader& block,
                  const std::vector<uint8_t>& payload, SessionData& out) {
  if (block.record_size != sizeof(TradeRec)) {
    out.warnings.push_back("trade block has record stride " +
                           std::to_string(block.record_size) + " (expected " +
                           std::to_string(sizeof(TradeRec)) + "); skipping");
    return;
  }

  for (uint32_t i = 0; i < block.record_count; ++i) {
    TradeRec rec{};
    std::memcpy(&rec,
                payload.data() + static_cast<size_t>(i) * block.record_size,
                sizeof(rec));

    if (!std::isfinite(rec.price) || !(rec.price > 0.0) ||
        !std::isfinite(rec.qty) || !(rec.qty > 0.0)) {
      ++out.trades_skipped;
      continue;
    }
    // Unknown is not a side. The `== Sell` test below would quietly file it as
    // an aggressive buy, inverting the footprint of every such print -- worse
    // than dropping it and counting the drop.
    if (rec.aggressor != static_cast<uint8_t>(Aggressor::Buy) &&
        rec.aggressor != static_cast<uint8_t>(Aggressor::Sell)) {
      ++out.trades_skipped;
      continue;
    }

    TradeEvent trade{};
    trade.timestamp = NsToMs(rec.ts_exchange_ns);
    trade.price = rec.price;
    trade.quantity = rec.qty;
    trade.is_buyer_maker =
        (rec.aggressor == static_cast<uint8_t>(Aggressor::Sell));
    out.trades.push_back(trade);
  }
}

void DecodeSnapshots(const BlockHeader& block,
                     const std::vector<uint8_t>& payload, SessionData& out,
                     OrderBook& book, const bool& deltas_seen,
                     int64_t& last_snapshot_seq) {
  if (block.record_size < sizeof(BookSnapshotRec) ||
      (block.record_size - sizeof(BookSnapshotRec)) % sizeof(BookLevel) != 0) {
    out.warnings.push_back(
        "book snapshot block has a stride that is not a whole number of levels "
        "(" + std::to_string(block.record_size) + "); skipping");
    return;
  }
  const size_t capacity =
      (block.record_size - sizeof(BookSnapshotRec)) / sizeof(BookLevel);

  for (uint32_t i = 0; i < block.record_count; ++i) {
    const uint8_t* record =
        payload.data() + static_cast<size_t>(i) * block.record_size;

    BookSnapshotRec rec{};
    std::memcpy(&rec, record, sizeof(rec));

    const size_t declared =
        static_cast<size_t>(rec.n_bids) + static_cast<size_t>(rec.n_asks);
    if (declared > capacity) {
      out.warnings.push_back("book snapshot claims " +
                             std::to_string(declared) +
                             " levels but its block holds " +
                             std::to_string(capacity) + "; skipping record");
      ++out.l2_skipped;
      continue;
    }

    // memcpy per level rather than a cast: the payload sits at
    // sizeof(BookSnapshotRec) into a record whose stride is a multiple of 8, so
    // it is aligned in practice, but a future format version is free to put it
    // somewhere else and a cast would become undefined behaviour silently.
    const size_t n_bids = std::min<size_t>(rec.n_bids, MAX_L2_DEPTH);
    const size_t n_asks = std::min<size_t>(rec.n_asks, MAX_L2_DEPTH);

    L2Snapshot snap{};
    snap.timestamp = NsToMs(rec.ts_exchange_ns);
    for (size_t b = 0; b < n_bids; ++b) {
      BookLevel level{};
      std::memcpy(&level,
                  record + sizeof(BookSnapshotRec) + b * sizeof(BookLevel),
                  sizeof(level));
      snap.bids[b].price = level.price;
      snap.bids[b].qty = level.qty;
    }
    for (size_t a = 0; a < n_asks; ++a) {
      BookLevel level{};
      std::memcpy(&level,
                  record + sizeof(BookSnapshotRec) +
                      (static_cast<size_t>(rec.n_bids) + a) * sizeof(BookLevel),
                  sizeof(level));
      snap.asks[a].price = level.price;
      snap.asks[a].qty = level.qty;
    }

    // One side empty is a resync in progress, not a market state. The depth and
    // stacked-imbalance triggers index bids[1]/asks[1]; publishing a half book
    // would make them read a crossed market of zeroes.
    if (!(snap.bids[0].price > 0.0) || !(snap.asks[0].price > 0.0)) {
      ++out.l2_skipped;
      continue;
    }
    snap.best_bid_price = snap.bids[0].price;
    snap.best_bid_qty = snap.bids[0].qty;
    snap.best_ask_price = snap.asks[0].price;
    snap.best_ask_qty = snap.asks[0].qty;

    out.l2.push_back(snap);
    last_snapshot_seq = rec.seq;

    // Keep the delta book in step with what was emitted, but only after the
    // file has proven it carries deltas: rebuilding the maps for every snapshot
    // doubles the cost of reading a snapshot-only recording, which is what the
    // recorder writes today.
    //
    // deltas_seen is a reference, not a copy. The writer's book block holds
    // snapshots and deltas and flushes on a kind change, so a file can go
    // snapshot -> delta -> snapshot; with a by-value flag the snapshot after a
    // delta left the reconstructed book at the FIRST snapshot, and every later
    // delta applied to a book that had already been superseded. Snapshot-only
    // files never reach that path, which is why it stayed hidden.
    if (deltas_seen) {
      std::vector<OrderBook::Level> bids;
      std::vector<OrderBook::Level> asks;
      bids.reserve(n_bids);
      asks.reserve(n_asks);
      for (size_t b = 0; b < n_bids; ++b) {
        bids.emplace_back(snap.bids[b].price, snap.bids[b].qty);
      }
      for (size_t a = 0; a < n_asks; ++a) {
        asks.emplace_back(snap.asks[a].price, snap.asks[a].qty);
      }
      book.ApplySnapshot(bids, asks, rec.seq);
    }
  }
}

void DecodeDeltas(const BlockHeader& block,
                  const std::vector<uint8_t>& payload, SessionData& out,
                  OrderBook& book, bool& deltas_seen,
                  int64_t last_snapshot_seq) {
  if (block.record_size < sizeof(BookDeltaRec) ||
      (block.record_size - sizeof(BookDeltaRec)) % sizeof(BookChange) != 0) {
    out.warnings.push_back(
        "book delta block has a stride that is not a whole number of changes (" +
        std::to_string(block.record_size) + "); skipping");
    return;
  }
  const size_t capacity =
      (block.record_size - sizeof(BookDeltaRec)) / sizeof(BookChange);

  // First delta in the file: seed the book from the last snapshot emitted.
  // Deltas are relative by definition, so without this they apply to nothing
  // and the book would silently lose every level they do not mention.
  if (!deltas_seen) {
    deltas_seen = true;
    if (out.l2.empty()) {
      out.warnings.push_back(
          "book delta block appears before any snapshot; the deltas cannot be "
          "applied and are skipped");
      return;
    }
    const L2Snapshot& last = out.l2.back();
    std::vector<OrderBook::Level> bids;
    std::vector<OrderBook::Level> asks;
    for (int i = 0; i < MAX_L2_DEPTH && last.bids[i].price > 0.0; ++i) {
      bids.emplace_back(last.bids[i].price, last.bids[i].qty);
    }
    for (int i = 0; i < MAX_L2_DEPTH && last.asks[i].price > 0.0; ++i) {
      asks.emplace_back(last.asks[i].price, last.asks[i].qty);
    }
    book.ApplySnapshot(bids, asks, last_snapshot_seq);
  }

  for (uint32_t i = 0; i < block.record_count; ++i) {
    const uint8_t* record =
        payload.data() + static_cast<size_t>(i) * block.record_size;

    BookDeltaRec rec{};
    std::memcpy(&rec, record, sizeof(rec));

    if (rec.n_changes > capacity) {
      out.warnings.push_back("book delta claims " +
                             std::to_string(rec.n_changes) +
                             " changes but its block holds " +
                             std::to_string(capacity) + "; skipping record");
      ++out.l2_skipped;
      continue;
    }

    std::vector<OrderBook::Level> bids;
    std::vector<OrderBook::Level> asks;
    for (uint16_t c = 0; c < rec.n_changes; ++c) {
      BookChange change{};
      std::memcpy(&change,
                  record + sizeof(BookDeltaRec) +
                      static_cast<size_t>(c) * sizeof(BookChange),
                  sizeof(change));

      // The format expresses "level left the book" as an explicit remove, while
      // OrderBook uses qty <= 0. Translate at this boundary rather than teaching
      // the book a second removal convention.
      const double qty =
          (change.action == book_change_action::kRemove) ? 0.0 : change.qty;
      if (change.side == book_change_side::kBid) {
        bids.emplace_back(change.price, qty);
      } else if (change.side == book_change_side::kAsk) {
        asks.emplace_back(change.price, qty);
      }
      // Unknown side values are skipped: a future format may add a third book,
      // and guessing where it belongs would corrupt the two we know.
    }

    const OrderBook::ApplyResult result = book.ApplyDelta(bids, asks, rec.seq);
    if (result == OrderBook::ApplyResult::GapDetected) {
      out.warnings.push_back(
          "book delta at seq " + std::to_string(rec.seq) +
          " skips a sequence number; dropping the reconstructed book until the "
          "next snapshot");
      book.Clear();
      continue;
    }
    if (result != OrderBook::ApplyResult::Applied) {
      ++out.l2_skipped;
      continue;
    }
    if (book.empty()) {
      // A delta drained one side: the book is unknown until a snapshot.
      ++out.l2_skipped;
      continue;
    }
    out.l2.push_back(book.ToSnapshot(NsToMs(rec.ts_exchange_ns)));
  }
}

bool DecodeBlocks(std::ifstream& in, const FileLayout& layout,
                  SessionData& out) {
  std::vector<uint8_t> payload;

  // Only materialised when the file actually carries deltas -- see
  // DecodeSnapshots for why the snapshot-only path skips the maps.
  OrderBook book;
  bool deltas_seen = false;
  int64_t last_snapshot_seq = 0;

  uint64_t offset = layout.header.header_size;
  while (offset + kBlockHeaderSize <= layout.data_end) {
    BlockHeader block{};
    if (!ReadExactAt(in, offset, &block, sizeof(block))) {
      out.error = "short read at block header, offset " +
                  std::to_string(offset);
      return false;
    }
    if (block.magic != kBlockMagic) {
      out.error = "block magic mismatch at offset " + std::to_string(offset) +
                  ": the file is corrupt or was not written by this format";
      return false;
    }

    const uint64_t payload_offset = offset + kBlockHeaderSize;
    if (block.payload_size > kMaxPayloadBytes ||
        payload_offset + block.payload_size > layout.data_end) {
      out.warnings.push_back(
          "block at offset " + std::to_string(offset) + " claims " +
          std::to_string(block.payload_size) +
          " payload bytes past the end of the recording; stopping here "
          "(truncated write)");
      break;
    }
    const uint64_t next = payload_offset + block.payload_size;

    payload.assign(static_cast<size_t>(block.payload_size), 0);
    if (block.payload_size > 0 &&
        !ReadExactAt(in, payload_offset, payload.data(), payload.size())) {
      out.error = "short read at block payload, offset " +
                  std::to_string(payload_offset);
      return false;
    }

    if (Crc32c(payload.data(), payload.size()) != block.payload_crc32c) {
      // Skip the block and keep going: block boundaries are explicit, so the
      // next header is still exactly where this one says it ends. If the size
      // field itself was the damaged part, the magic check on the next
      // iteration catches the desync and stops with an error.
      out.warnings.push_back(
          "block at offset " + std::to_string(offset) +
          " failed its checksum; skipping " +
          std::to_string(block.record_count) + " record(s)");
      offset = next;
      continue;
    }
    if (block.codec != static_cast<uint16_t>(BlockCodec::None)) {
      out.warnings.push_back(
          "block at offset " + std::to_string(offset) + " uses block codec " +
          std::to_string(block.codec) +
          ", which this reader does not implement; skipping");
      offset = next;
      continue;
    }

    ++out.blocks_read;
    out.records_read += block.record_count;

    switch (static_cast<RecordKind>(block.kind)) {
      case RecordKind::Trade:
        DecodeTrades(block, payload, out);
        break;
      case RecordKind::BookSnapshot:
        DecodeSnapshots(block, payload, out, book, deltas_seen,
                        last_snapshot_seq);
        break;
      case RecordKind::BookDelta:
        DecodeDeltas(block, payload, out, book, deltas_seen,
                     last_snapshot_seq);
        break;
      case RecordKind::Mark:
      case RecordKind::SessionMarker:
        // Reserved kinds. A reader skips what it does not know rather than
        // guess, so a future writer adding funding marks does not make this
        // build reject the file.
        break;
      default:
        out.warnings.push_back("block at offset " + std::to_string(offset) +
                               " has unknown record kind " +
                               std::to_string(block.kind) + "; skipping");
        break;
    }

    offset = next;
  }
  return true;
}

}  // namespace

bool IsSessionFile(const std::string& path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) return false;

  std::ifstream in(path, std::ios::binary);
  if (!in) return false;

  char magic[sizeof(kFileMagic)] = {};
  if (!in.read(magic, sizeof(magic))) return false;
  return std::memcmp(magic, kFileMagic, sizeof(magic)) == 0;
}

SessionData ReadSessionFile(const std::string& path) {
  SessionData out;

  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    out.error = "cannot open '" + path + "'";
    return out;
  }
  const std::streamoff size = in.tellg();
  if (size < 0) {
    out.error = "cannot determine the size of '" + path + "'";
    return out;
  }
  const uint64_t file_size = static_cast<uint64_t>(size);

  FileLayout layout;
  if (!ReadFileHeader(in, file_size, layout, out.error)) return out;

  ParseMetadata(in, layout, file_size, out);
  if (!DecodeBlocks(in, layout, out)) {
    // Keep the partial result: an hour of book in front of a corrupt tail is
    // still an hour the backtest can run on.
  }

  // Order within each channel is preserved by a stable sort. The replay helpers
  // stop scanning once they pass `end_time`, which is only correct on an
  // ascending vector, and two prints in the same millisecond must keep their
  // arrival order because clustered/footprint triggers depend on it.
  std::stable_sort(out.trades.begin(), out.trades.end(),
                   [](const TradeEvent& a, const TradeEvent& b) {
                     return a.timestamp < b.timestamp;
                   });
  std::stable_sort(out.l2.begin(), out.l2.end(),
                   [](const L2Snapshot& a, const L2Snapshot& b) {
                     return a.timestamp < b.timestamp;
                   });

  if (out.trades_skipped > 0) {
    out.warnings.push_back(
        std::to_string(out.trades_skipped) +
        " trade record(s) skipped: unknown aggressor or non-positive price/qty");
  }
  if (out.l2_skipped > 0) {
    out.warnings.push_back(
        std::to_string(out.l2_skipped) +
        " book record(s) skipped: unreadable stride, one-sided book, or a delta "
        "with no snapshot behind it");
  }
  return out;
}

}  // namespace mokuren::mdf
