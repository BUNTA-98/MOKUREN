#include "market_data/reader.hpp"

#include "market_data/crc32c.hpp"
#include "market_data/writer.hpp"   // AppendBlock / is_open / last_error
#include "order_book.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
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

// "0x%08x" for a 32-bit field a report has to show verbatim. A decimal print is
// useless when the value is being compared against kBlockMagic or read out of a
// hex dump, which is the only reason this exists.
std::string Hex32(uint32_t v) {
  char buf[11];
  std::snprintf(buf, sizeof buf, "0x%08x", static_cast<unsigned>(v));
  return buf;
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
    out.meta.writer_version = json.value("writer_version", std::string{});
  } catch (const std::exception& e) {
    // A present-but-wrong-typed field is metadata damage, not a reason to
    // refuse the blocks: the records are what the backtest runs on.
    out.warnings.push_back(std::string("metadata field has an unexpected "
                                       "type: ") + e.what());
  }
}

// Emit receives each validated TradeEvent. The materialising reader pushes
// into out.trades; SessionCursor pushes into its reorder buffer. One body so
// the two paths cannot disagree about what a valid trade is.
template <typename Emit>
void DecodeTradesImpl(const BlockHeader& block,
                      const std::vector<uint8_t>& payload, SessionData& out,
                      Emit&& emit) {
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
    emit(trade);
  }
}

void DecodeTrades(const BlockHeader& block,
                  const std::vector<uint8_t>& payload, SessionData& out) {
  DecodeTradesImpl(block, payload, out,
                   [&out](TradeEvent t) { out.trades.push_back(t); });
}

// Emit receives each validated snapshot. Both readers get the same stride
// check, the same one-sided-book rejection, and the same delta-book
// bookkeeping -- only the sink differs.
template <typename Emit>
void DecodeSnapshotsImpl(const BlockHeader& block,
                         const std::vector<uint8_t>& payload, SessionData& out,
                         OrderBook& book, const bool& deltas_seen,
                         int64_t& last_snapshot_seq, Emit&& emit) {
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

    emit(snap);
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

void DecodeSnapshots(const BlockHeader& block,
                     const std::vector<uint8_t>& payload, SessionData& out,
                     OrderBook& book, const bool& deltas_seen,
                     int64_t& last_snapshot_seq) {
  DecodeSnapshotsImpl(block, payload, out, book, deltas_seen,
                      last_snapshot_seq,
                      [&out](L2Snapshot s) { out.l2.push_back(s); });
}

// `last_snapshot` is the snapshot to seed the book from on the first delta.
// The materialising path passes &out.l2.back(); SessionCursor passes the last
// snapshot it decoded. nullptr means "there is no snapshot behind these
// deltas" and is the same failure the materialising path already warns about.
template <typename Emit>
void DecodeDeltasImpl(const BlockHeader& block,
                      const std::vector<uint8_t>& payload, SessionData& out,
                      OrderBook& book, bool& deltas_seen,
                      int64_t last_snapshot_seq,
                      const L2Snapshot* last_snapshot, Emit&& emit) {
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
    if (last_snapshot == nullptr) {
      out.warnings.push_back(
          "book delta block appears before any snapshot; the deltas cannot be "
          "applied and are skipped");
      return;
    }
    const L2Snapshot& last = *last_snapshot;
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
    emit(book.ToSnapshot(NsToMs(rec.ts_exchange_ns)));
  }
}

void DecodeDeltas(const BlockHeader& block,
                  const std::vector<uint8_t>& payload, SessionData& out,
                  OrderBook& book, bool& deltas_seen,
                  int64_t last_snapshot_seq) {
  const L2Snapshot* last = out.l2.empty() ? nullptr : &out.l2.back();
  DecodeDeltasImpl(block, payload, out, book, deltas_seen, last_snapshot_seq,
                   last,
                   [&out](L2Snapshot s) { out.l2.push_back(s); });
}

bool DecodeBlocks(std::ifstream& in, const FileLayout& layout,
                  SessionData& out, int64_t start_ns, int64_t end_ns,
                  Channel mask) {
  std::vector<uint8_t> payload;

  // Set when a delta block before the requested range had to be skipped. It is
  // reported once at the end rather than per block: the caller needs to know
  // the reconstructed book may start incomplete, not how many blocks it missed.
  bool delta_skipped_before_range = false;

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
      ++out.blocks_skipped;
      break;
    }
    const uint64_t next = payload_offset + block.payload_size;

    // Range gate. After the bounds check, so a corrupt size field cannot make a
    // range read skip over a real block; before the payload read, so it costs
    // only the bytes it actually decodes -- which is the entire point.
    //
    // Sound because a block's [first_ts_ns, last_ts_ns] brackets every record
    // in it: a block entirely outside the range cannot hold a record inside it,
    // whatever order the blocks themselves are in.
    if (block.last_ts_ns < start_ns || block.first_ts_ns > end_ns) {
      if (static_cast<RecordKind>(block.kind) == RecordKind::BookDelta &&
          block.first_ts_ns < start_ns) {
        delta_skipped_before_range = true;
      }
      offset = next;
      continue;
    }

    // Channel filter, applied BEFORE the payload is read: a caller that only
    // wants trades must not pay the crc and the copy for a day's book, which
    // is the whole reason the mask exists.
    const RecordKind mask_kind = static_cast<RecordKind>(block.kind);
    if ((mask_kind == RecordKind::Trade &&
         !HasChannel(mask, Channel::Trades)) ||
        ((mask_kind == RecordKind::BookSnapshot ||
          mask_kind == RecordKind::BookDelta) &&
         !HasChannel(mask, Channel::L2))) {
      offset = next;
      continue;
    }

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
      ++out.blocks_skipped;
      offset = next;
      continue;
    }
    if (block.codec != static_cast<uint16_t>(BlockCodec::None)) {
      out.warnings.push_back(
          "block at offset " + std::to_string(offset) + " uses block codec " +
          std::to_string(block.codec) +
          ", which this reader does not implement; skipping");
      ++out.blocks_skipped;
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

  if (delta_skipped_before_range) {
    out.warnings.push_back(
        "range read skipped book delta blocks before the requested start; the "
        "reconstructed book at the start of the range may be incomplete");
  }
  return true;
}

// Largest same-kind step backwards between two adjacent index entries, in
// physical (block_offset) order. Same measurement Reader::Open takes to fill
// Meta::max_inversion_ns, factored out so SessionCursor can size its reorder
// window without going through the merger-facing Reader.
int64_t MaxInversionFromIndex(const std::vector<IndexEntry>& index) {
  int64_t last_ts[3] = {0, 0, 0};
  bool    seen[3]    = {false, false, false};
  int64_t max_inv    = 0;
  for (const auto& e : index) {
    int slot = -1;
    switch (static_cast<RecordKind>(e.kind)) {
      case RecordKind::Trade:        slot = 0; break;
      case RecordKind::BookSnapshot: slot = 1; break;
      case RecordKind::BookDelta:    slot = 2; break;
      default:                       continue;
    }
    if (seen[slot]) {
      const int64_t back = last_ts[slot] - e.first_ts_ns;
      if (back > max_inv) max_inv = back;
    }
    seen[slot]    = true;
    last_ts[slot] = e.last_ts_ns;
  }
  return max_inv;
}

}  // namespace

bool Reader::Open(const std::string& path) {
  // Reset a previous Open: callers reuse the object to walk a directory.
  in_ = std::ifstream{};
  header_ = FileHeader{};
  file_size_ = 0;
  data_end_ = 0;
  meta_ = Meta{};
  index_.clear();
  copied_records_ = 0;
  index_mismatches_ = 0;
  blocks_walked_ = 0;
  error_.clear();

  in_.open(path, std::ios::binary | std::ios::ate);
  if (!in_) {
    error_ = "cannot open '" + path + "'";
    return false;
  }
  const std::streamoff size = in_.tellg();
  if (size < 0) {
    error_ = "cannot determine the size of '" + path + "'";
    return false;
  }
  const uint64_t file_size = static_cast<uint64_t>(size);
  file_size_ = file_size;

  FileLayout layout;
  if (!ReadFileHeader(in_, file_size, layout, error_)) return false;
  header_ = layout.header;
  data_end_ = layout.data_end;
  meta_.first_block_offset = header_.header_size;

  // Reuse ParseMetadata instead of duplicating the JSON shape. It fills a
  // SessionData on purpose; a Reader that parsed json differently would drift
  // from ReadSessionFile the first time a field is added.
  SessionData sd;
  ParseMetadata(in_, layout, file_size, sd);
  meta_.venue           = sd.meta.venue;
  meta_.env             = sd.meta.env;
  meta_.category        = sd.meta.category;
  meta_.symbol          = sd.meta.symbol;
  meta_.source          = sd.meta.source;
  meta_.depth_per_side  = sd.meta.depth_per_side;
  meta_.created_unix_ns = sd.meta.created_unix_ns;
  meta_.writer_version  = sd.meta.writer_version;

  // Finalised == trailer exists. A file whose Close() never ran leaves
  // metadata_size and index_size at zero, which is how the merger tells "a
  // crash" apart from "a complete day".
  meta_.finalised = header_.metadata_size > 0 && header_.index_size > 0;
  if (!meta_.finalised) return true;

  if (header_.index_size % kIndexEntrySize != 0 ||
      header_.index_offset < header_.header_size ||
      header_.index_offset + header_.index_size > file_size) {
    error_ = "index block lies outside the file or has a bad size";
    return false;
  }
  index_.resize(static_cast<size_t>(header_.index_size / kIndexEntrySize));
  if (!index_.empty() &&
      !ReadExactAt(in_, header_.index_offset, index_.data(),
                   static_cast<size_t>(header_.index_size))) {
    error_ = "short read at index block";
    return false;
  }

  if (!index_.empty()) {
    meta_.first_ts_ns = index_[0].first_ts_ns;
    meta_.last_ts_ns  = index_[0].last_ts_ns;
    for (const auto& e : index_) {
      if (e.first_ts_ns < meta_.first_ts_ns) meta_.first_ts_ns = e.first_ts_ns;
      if (e.last_ts_ns  > meta_.last_ts_ns)  meta_.last_ts_ns  = e.last_ts_ns;
      meta_.record_count += e.record_count;
    }

    // CopyPayloadTo copies in block_offset order, so the ordering claim has to
    // be measured in that same order. Sorting here makes it the member's
    // invariant; CopyPayloadTo still sorts as a defensive no-op.
    std::sort(index_.begin(), index_.end(),
              [](const IndexEntry& a, const IndexEntry& b) {
                return a.block_offset < b.block_offset;
              });
    // PER KIND, never across the whole index. Writer::FlushAll writes trades
    // before book, and each stream is flushed independently as it fills (see
    // writer.hpp: "blocks of different kinds therefore appear in flush order,
    // not arrival order"). A trade block and a snapshot block from the same
    // second therefore interleave and overlap by construction, so comparing
    // adjacent entries regardless of kind compares two unrelated clocks and
    // rejects every real recording.
    int64_t last_ts[3] = {0, 0, 0};
    bool    seen[3]    = {false, false, false};
    for (const auto& e : index_) {
      int slot = -1;
      switch (static_cast<RecordKind>(e.kind)) {
        case RecordKind::Trade:        slot = 0; break;
        case RecordKind::BookSnapshot: slot = 1; break;
        case RecordKind::BookDelta:    slot = 2; break;
        default:                       continue;  // reserved kinds carry no order
      }
      // Measure, do not judge: the tolerated size is a policy the merger owns
      // (MergerConfig::max_skew_ns), because it depends on what the caller is
      // willing to treat as a late print. This layer only reports the number.
      if (seen[slot]) {
        const int64_t back = last_ts[slot] - e.first_ts_ns;
        if (back > meta_.max_inversion_ns) {
          meta_.max_inversion_ns      = back;
          meta_.max_inversion_prev_ns = last_ts[slot];
          meta_.max_inversion_next_ns = e.first_ts_ns;
          meta_.max_inversion_kind    = e.kind;
        }
      }
      seen[slot]    = true;
      last_ts[slot] = e.last_ts_ns;
    }

    // Lowermost entry offset, after the sort above, for the chain-vs-index
    // comparison in --stats.
    meta_.first_index_offset = index_.front().block_offset;
  }
  return true;
}

bool Reader::CopyPayloadTo(Writer& out) {
  if (!in_.is_open()) {
    error_ = "reader is not open";
    return false;
  }
  if (!out.is_open()) {
    error_ = "destination writer is not open";
    return false;
  }

  copied_records_ = 0;
  index_mismatches_ = 0;

  // The block chain between header_size and data_end is the source of truth;
  // the index is a cache over it. Walking the chain is what DecodeBlocks does,
  // and trusting the index here was the anomaly: index entries are NOT covered
  // by any checksum (only payloads are), so one damaged entry was reported as a
  // corrupt payload -- "block magic mismatch" from a file whose data is intact
  // -- and an index that had lost entries silently copied fewer blocks than the
  // file holds. The walk costs nothing extra: the payload is read either way and
  // the header is 56 bytes.
  //
  // Sorted so the cross-check below advances through both in one pass. Open()
  // already sorted it; repeating that is free and keeps this function from
  // depending on it.
  std::sort(index_.begin(), index_.end(),
            [](const IndexEntry& a, const IndexEntry& b) {
              return a.block_offset < b.block_offset;
            });

  std::vector<uint8_t> payload;
  size_t next_index = 0;
  uint64_t offset = header_.header_size;

  while (offset + kBlockHeaderSize <= data_end_) {
    BlockHeader bh{};
    if (!ReadExactAt(in_, offset, &bh, sizeof(bh))) {
      error_ = "short read at block header, offset " + std::to_string(offset);
      return false;
    }
    if (bh.magic != kBlockMagic) {
      // Print the bytes found. "Magic mismatch" alone cannot separate a torn
      // write from a pointer in the wrong place, and the four bytes usually can.
      error_ = "block magic mismatch at offset " + std::to_string(offset) +
               " (found " + Hex32(bh.magic) + ", expected " +
               Hex32(kBlockMagic) + ")";
      return false;
    }
    // payload_size comes from the file, not from the index, so a corrupt size
    // field must not decide the allocation or the read extent. Bounded by
    // data_end_, not file_size_: the trailer sits past data_end_, so a block
    // claiming to extend into it is damaged even though those bytes exist.
    if (bh.payload_size > kMaxPayloadBytes ||
        offset + kBlockHeaderSize + bh.payload_size > data_end_) {
      error_ = "block at offset " + std::to_string(offset) + " claims " +
               std::to_string(bh.payload_size) +
               " payload bytes past the end of the recording";
      return false;
    }
    if (bh.codec != static_cast<uint16_t>(BlockCodec::None)) {
      error_ = "block at offset " + std::to_string(offset) +
               " uses block codec " + std::to_string(bh.codec) +
               "; this build only merges uncompressed blocks";
      return false;
    }

    const uint64_t payload_offset = offset + kBlockHeaderSize;
    payload.assign(static_cast<size_t>(bh.payload_size), 0);
    if (bh.payload_size > 0 &&
        !ReadExactAt(in_, payload_offset, payload.data(), payload.size())) {
      error_ = "short read at block payload, offset " +
               std::to_string(payload_offset);
      return false;
    }
    if (Crc32c(payload.data(), payload.size()) != bh.payload_crc32c) {
      error_ = "block at offset " + std::to_string(offset) +
               " failed its checksum; refusing to merge damaged data";
      return false;
    }
    // The destination metadata describes record_sizes from the writer's own
    // Options, but block layouts are copied verbatim from the source. A file
    // written with a different stride would be described wrongly, so fail
    // rather than emit a file that misreports itself. TradeRec is fixed-size
    // and needs no check.
    //
    // Bound to Writer::Options{} defaults on purpose: MergeWindow opens the
    // writer with the default Options, so these are the strides the merged
    // file will describe. If the merger ever accepts Writer::Options, pass
    // them here instead of constructing a default.
    const RecordKind kind = static_cast<RecordKind>(bh.kind);

    // The merger's seam argument (see merger.hpp) requires that no book state
    // crosses a block boundary: a reader rebuilding the merged book from the
    // first snapshot it sees must end up in the same state it would have in
    // the daily source. That holds only because the recorder writes full
    // snapshots and never deltas. A source that does carry deltas would merge
    // into a file whose delta stream has no guaranteed preceding snapshot, so
    // reject the kind rather than emit a file that violates the precondition.
    if (kind == RecordKind::BookDelta) {
      error_ = "block at offset " + std::to_string(offset) +
               " is a book delta; the merger only concatenates snapshot-only "
               "recordings";
      return false;
    }
    const unsigned expected_snapshot = static_cast<unsigned>(
        sizeof(BookSnapshotRec) +
        static_cast<size_t>(Writer::Options{}.max_levels_per_side) * 2 *
            sizeof(BookLevel));
    if (kind == RecordKind::BookSnapshot &&
        bh.record_size != expected_snapshot) {
      error_ = "block at offset " + std::to_string(offset) +
               " has record stride " + std::to_string(bh.record_size) +
               ", which does not match this merger's layout; refusing to merge";
      return false;
    }
    if (!out.AppendBlock(bh, payload.data(), payload.size())) {
      error_ = "append block: " + out.last_error();
      return false;
    }

    // Cross-check the trailer as we go. An entry that points before this block
    // was found never matched a block start, so the index is damaged or stale --
    // but the payload came from the bytes, so that is a repair, not a failure.
    while (next_index < index_.size() &&
           index_[next_index].block_offset < offset) {
      ++index_mismatches_;
      ++next_index;
    }
    if (next_index < index_.size() &&
        index_[next_index].block_offset == offset) {
      ++next_index;
    }

    ++blocks_walked_;
    copied_records_ += bh.record_count;
    offset = payload_offset + bh.payload_size;
  }

  // Entries left over point past the last block the chain reached.
  index_mismatches_ += index_.size() - next_index;
  return true;
}

bool Reader::ScanChainBounds() {
  if (!in_.is_open()) {
    error_ = "reader is not open";
    return false;
  }

  index_mismatches_ = 0;
  blocks_walked_ = 0;
  size_t next_index = 0;

  int64_t  first_ts = 0;
  int64_t  last_ts  = 0;
  bool     any      = false;
  uint64_t count    = 0;

  // Per stream, exactly as Open() measures it from the index: trades and book
  // are flushed independently and interleave, so only same-kind blocks can be
  // compared without comparing two unrelated clocks.
  int64_t last_kind_ts[3] = {0, 0, 0};
  bool    seen_kind[3]    = {false, false, false};

  int64_t  max_inv  = 0;
  int64_t  inv_prev = 0;
  int64_t  inv_next = 0;
  uint16_t inv_kind = 0;

  uint64_t offset = header_.header_size;
  while (offset + kBlockHeaderSize <= data_end_) {
    BlockHeader bh{};
    if (!ReadExactAt(in_, offset, &bh, sizeof(bh))) {
      error_ = "short read at block header, offset " + std::to_string(offset);
      return false;
    }
    if (bh.magic != kBlockMagic) {
      error_ = "block magic mismatch at offset " + std::to_string(offset) +
               " (found " + Hex32(bh.magic) + ", expected " +
               Hex32(kBlockMagic) + ")";
      return false;
    }
    // Same two conditions as CopyPayloadTo. Bounded by data_end_, not
    // file_size_: the trailer sits past data_end_, so a block claiming to
    // extend into it is damaged even though those bytes exist.
    if (bh.payload_size > kMaxPayloadBytes ||
        offset + kBlockHeaderSize + bh.payload_size > data_end_) {
      error_ = "block at offset " + std::to_string(offset) + " claims " +
               std::to_string(bh.payload_size) +
               " payload bytes past the end of the recording";
      return false;
    }

    // The FILE's span, both streams together: that is what orders the inputs
    // and labels the merged output.
    if (!any) {
      first_ts = bh.first_ts_ns;
      last_ts  = bh.last_ts_ns;
      any      = true;
    } else {
      if (bh.first_ts_ns < first_ts) first_ts = bh.first_ts_ns;
      if (bh.last_ts_ns  > last_ts)  last_ts  = bh.last_ts_ns;
    }
    count += bh.record_count;
    ++blocks_walked_;

    int slot = -1;
    switch (static_cast<RecordKind>(bh.kind)) {
      case RecordKind::Trade:        slot = 0; break;
      case RecordKind::BookSnapshot: slot = 1; break;
      case RecordKind::BookDelta:    slot = 2; break;
      default:                       slot = -1; break;
    }
    if (slot >= 0) {
      if (seen_kind[slot]) {
        const int64_t back = last_kind_ts[slot] - bh.first_ts_ns;
        if (back > max_inv) {
          max_inv  = back;
          inv_prev = last_kind_ts[slot];
          inv_next = bh.first_ts_ns;
          inv_kind = bh.kind;
        }
      }
      seen_kind[slot]    = true;
      last_kind_ts[slot] = bh.last_ts_ns;
    }

    // Cross-check the trailer here too, so --stats can report a damaged index
    // without a merge having to fail first. index_ is sorted by block_offset,
    // so both advance in one pass; an entry pointing before this block was
    // found never matched a block start.
    while (next_index < index_.size() &&
           index_[next_index].block_offset < offset) {
      ++index_mismatches_;
      ++next_index;
    }
    if (next_index < index_.size() &&
        index_[next_index].block_offset == offset) {
      ++next_index;
    }

    // Seek past the payload rather than read it: ReadExactAt positions the
    // stream anyway, so this pass never touches a record byte.
    offset += kBlockHeaderSize + bh.payload_size;
  }

  // Entries left over point past the last block the chain reached.
  index_mismatches_ += index_.size() - next_index;

  if (any) {
    meta_.first_ts_ns = first_ts;
    meta_.last_ts_ns  = last_ts;
  }
  meta_.record_count          = count;
  meta_.max_inversion_ns      = max_inv;
  meta_.max_inversion_prev_ns = inv_prev;
  meta_.max_inversion_next_ns = inv_next;
  meta_.max_inversion_kind    = inv_kind;
  return true;
}

bool IsSessionFile(const std::string& path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) return false;

  std::ifstream in(path, std::ios::binary);
  if (!in) return false;

  char magic[sizeof(kFileMagic)] = {};
  if (!in.read(magic, sizeof(magic))) return false;
  return std::memcmp(magic, kFileMagic, sizeof(magic)) == 0;
}

SessionData ReadSessionFile(const std::string& path, Channel mask) {
  return ReadSessionFileRange(path, std::numeric_limits<int64_t>::min(),
                              std::numeric_limits<int64_t>::max(), mask);
}

SessionData ReadSessionFileRange(const std::string& path, int64_t start_ns,
                                 int64_t end_ns, Channel mask) {
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
  if (!DecodeBlocks(in, layout, out, start_ns, end_ns, mask)) {
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

// --- SessionCursor ---------------------------------------------------------

namespace {

// Slack applied when the file has no measurable inversion: no index (a
// recording that was never finalised), or every stream strictly ascending.
// Buffering a few seconds is cheap -- a busy second is a few hundred trade
// records and a few dozen book frames -- and removes the failure mode where an
// unfinalised file emits out of order and SliceTrades truncates on the first
// inversion.
constexpr int64_t kDefaultCursorSlackNs = 5'000'000'000LL;

// Ceiling on reorder entries held per cursor. Never reached on a healthy file
// (the window is slack_ns worth of records); a corrupt slack field must not
// turn the cursor back into a full materialisation. 131k entries is ~5 minutes
// of a busy market's trades or ~3.5 hours of book frames -- enough margin that
// only a genuinely broken metadata figure touches it. When exceeded, the
// oldest entry is emitted anyway and reported once.
constexpr size_t kMaxReorderEntries = 131072;

}  // namespace

bool SessionCursor::Open(const std::string& path, Channel channel,
                         int64_t start_ns, int64_t end_ns) {
  channel_ = channel;
  start_ns_ = start_ns;
  end_ns_ = end_ns;
  status_ = SessionData{};

  // A cursor has one front and one Advance, so it cannot stream two channels
  // coherently. Callers wanting both open two cursors.
  if (channel != Channel::Trades && channel != Channel::L2) {
    status_.error =
        "SessionCursor: channel must be Trades or L2, not Both";
    return false;
  }

  // NsToMs the boundary. NsToMs negates to floor negatives, which overflows
  // on int64_t::min(); the unset bounds are the common case for a full-file
  // walk, so guard them here rather than forcing the caller to pass a
  // different sentinel.
  start_ms_ = (start_ns == std::numeric_limits<int64_t>::min())
                  ? std::numeric_limits<int64_t>::min()
                  : NsToMs(start_ns);
  end_ms_ = (end_ns == std::numeric_limits<int64_t>::max())
                ? std::numeric_limits<int64_t>::max()
                : NsToMs(end_ns);

  in_.open(path, std::ios::binary | std::ios::ate);
  if (!in_) {
    status_.error = "cannot open '" + path + "'";
    return false;
  }
  const std::streamoff size = in_.tellg();
  if (size < 0) {
    status_.error = "cannot determine the size of '" + path + "'";
    return false;
  }
  file_size_ = static_cast<uint64_t>(size);

  FileLayout layout;
  if (!ReadFileHeader(in_, file_size_, layout, status_.error)) return false;
  ParseMetadata(in_, layout, file_size_, status_);
  data_end_ = layout.data_end;
  cursor_offset_ = layout.header.header_size;

  // Size the reorder window from the file's OWN inversion measurement, not a
  // guessed constant. The floor keeps an unfinalised file (index_size == 0)
  // and a file whose blocks happen to be in physical order from emitting
  // before a late print has had a chance to arrive.
  slack_ns_ = kDefaultCursorSlackNs;
  if (layout.header.index_size > 0 &&
      layout.header.index_size % kIndexEntrySize == 0 &&
      layout.header.index_offset >= layout.header.header_size &&
      layout.header.index_offset + layout.header.index_size <= file_size_) {
    std::vector<IndexEntry> index(
        static_cast<size_t>(layout.header.index_size / kIndexEntrySize));
    if (!index.empty() &&
        ReadExactAt(in_, layout.header.index_offset, index.data(),
                    static_cast<size_t>(layout.header.index_size))) {
      // The index is written in block_offset order; sort defensively so a
      // damaged trailer cannot make the inversion figure meaningless.
      std::sort(index.begin(), index.end(),
                [](const IndexEntry& a, const IndexEntry& b) {
                  return a.block_offset < b.block_offset;
                });
      const int64_t measured = MaxInversionFromIndex(index);
      if (measured > slack_ns_) slack_ns_ = measured;
    }
  }

  return true;
}

void SessionCursor::InsertBuffered(Entry entry) {
  // lower_bound on (ts, seq): ties in ts are broken by arrival order, exactly
  // what stable_sort produces on the materialising path. seq is unique, so the
  // order is strict.
  auto pos = std::lower_bound(
      buffer_.begin(), buffer_.end(), entry,
      [](const Entry& existing, const Entry& value) {
        if (existing.ts != value.ts) return existing.ts < value.ts;
        return existing.seq < value.seq;
      });
  buffer_.insert(pos, std::move(entry));
  if (buffer_.size() > kMaxReorderEntries) {
    if (!overflow_reported_) {
      status_.warnings.push_back(
          "reorder buffer exceeded its cap; emitting records slightly out of "
          "order. The file's max_inversion_ns is implausibly large for a real "
          "recording.");
      overflow_reported_ = true;
    }
    EmitTop();
  }
}

void SessionCursor::EmitTop() {
  if (buffer_.empty()) return;
  const Entry& front = buffer_.front();
  if (front.is_l2) l2_ = front.l2;
  else             trade_ = front.trade;
  last_emitted_ts_ = front.ts;
  buffer_.pop_front();
  front_ready_ = true;
}

bool SessionCursor::HasNext() {
  if (front_ready_) return true;
  if (exhausted_ && buffer_.empty()) return false;

  // An entry at T can be followed by an unseen record with ts >= T - slack
  // (that is what slack measures), so nothing above T - slack can be emitted
  // yet. At EOF the remaining buffer is already in the correct order.
  while (!front_ready_) {
    if (!buffer_.empty() &&
        (exhausted_ || buffer_.front().ts <= newest_seen_ts_ - slack_ns_)) {
      EmitTop();
      return true;
    }
    if (!PullBlock()) {
      exhausted_ = true;
      if (buffer_.empty()) return false;
      // else: loop again; EmitTop fires because exhausted_ is now set and
      // short-circuits the slack check above.
    }
  }
  return true;
}

void SessionCursor::Advance() { front_ready_ = false; }

bool SessionCursor::PullBlock() {
  if (cursor_offset_ + kBlockHeaderSize > data_end_) return false;

  BlockHeader block{};
  if (!ReadExactAt(in_, cursor_offset_, &block, sizeof(block))) {
    status_.error = "short read at block header, offset " +
                    std::to_string(cursor_offset_);
    return false;
  }
  if (block.magic != kBlockMagic) {
    status_.error = "block magic mismatch at offset " +
                    std::to_string(cursor_offset_) +
                    ": the file is corrupt or was not written by this format";
    return false;
  }
  const uint64_t payload_offset = cursor_offset_ + kBlockHeaderSize;
  if (block.payload_size > kMaxPayloadBytes ||
      payload_offset + block.payload_size > data_end_) {
    status_.warnings.push_back(
        "block at offset " + std::to_string(cursor_offset_) + " claims " +
        std::to_string(block.payload_size) +
        " payload bytes past the end of the recording; stopping here "
        "(truncated write)");
    ++status_.blocks_skipped;
    cursor_offset_ = data_end_;
    return false;
  }
  const uint64_t next = payload_offset + block.payload_size;
  const RecordKind kind = static_cast<RecordKind>(block.kind);

  // Range gate (block level). A block entirely outside [start, end] cannot
  // hold a record inside it, whatever order the blocks are in.
  if (block.last_ts_ns < start_ns_ || block.first_ts_ns > end_ns_) {
    cursor_offset_ = next;
    return true;
  }

  // Channel gate. Rejects a whole block before the payload read so a trades
  // cursor does not pay the cache and the crc for a day's book frames.
  const bool kind_is_trade = (kind == RecordKind::Trade);
  const bool kind_is_book  = (kind == RecordKind::BookSnapshot ||
                              kind == RecordKind::BookDelta);
  if ((channel_ == Channel::Trades && !kind_is_trade) ||
      (channel_ == Channel::L2 && !kind_is_book)) {
    cursor_offset_ = next;
    return true;
  }

  payload_buf_.assign(static_cast<size_t>(block.payload_size), 0);
  if (block.payload_size > 0 &&
      !ReadExactAt(in_, payload_offset, payload_buf_.data(),
                   payload_buf_.size())) {
    status_.error = "short read at block payload, offset " +
                    std::to_string(payload_offset);
    return false;
  }
  if (Crc32c(payload_buf_.data(), payload_buf_.size()) != block.payload_crc32c) {
    status_.warnings.push_back(
        "block at offset " + std::to_string(cursor_offset_) +
        " failed its checksum; skipping " +
        std::to_string(block.record_count) + " record(s)");
    ++status_.blocks_skipped;
    cursor_offset_ = next;
    return true;
  }
  if (block.codec != static_cast<uint16_t>(BlockCodec::None)) {
    status_.warnings.push_back(
        "block at offset " + std::to_string(cursor_offset_) +
        " uses block codec " + std::to_string(block.codec) +
        ", which this reader does not implement; skipping");
    ++status_.blocks_skipped;
    cursor_offset_ = next;
    return true;
  }

  ++status_.blocks_read;
  status_.records_read += block.record_count;

  // The Emit callbacks below apply the range filter at RECORD granularity, not
  // block granularity: a block that straddles start_ms_ (or end_ms_) is
  // decoded whole and the individual records are filtered, which is what a
  // caller passing a window wants. The materialising path is block-atomic;
  // this one is record-precise, matching SliceTrades / SliceL2.
  switch (kind) {
    case RecordKind::Trade:
      DecodeTradesImpl(block, payload_buf_, status_, [this](TradeEvent t) {
        if (t.timestamp < start_ms_ || t.timestamp > end_ms_) return;
        if (t.timestamp > newest_seen_ts_) newest_seen_ts_ = t.timestamp;
        Entry e{};
        e.ts    = t.timestamp;
        e.seq   = next_arrival_seq_++;
        e.is_l2 = false;
        e.trade = t;
        InsertBuffered(std::move(e));
      });
      break;

    case RecordKind::BookSnapshot:
      DecodeSnapshotsImpl(block, payload_buf_, status_, book_, deltas_seen_,
                          last_snapshot_seq_, [this](L2Snapshot s) {
        // Track the seed regardless of range: a delta behind the range still
        // needs the last snapshot behind it, and that snapshot is with high
        // probability outside [start, end].
        last_decoded_snapshot_ = s;
        has_decoded_snapshot_ = true;
        if (s.timestamp < start_ms_ || s.timestamp > end_ms_) return;
        if (s.timestamp > newest_seen_ts_) newest_seen_ts_ = s.timestamp;
        Entry e{};
        e.ts    = s.timestamp;
        e.seq   = next_arrival_seq_++;
        e.is_l2 = true;
        e.l2    = s;
        InsertBuffered(std::move(e));
      });
      break;

    case RecordKind::BookDelta: {
      const L2Snapshot* seed =
          has_decoded_snapshot_ ? &last_decoded_snapshot_ : nullptr;
      DecodeDeltasImpl(block, payload_buf_, status_, book_, deltas_seen_,
                       last_snapshot_seq_, seed, [this](L2Snapshot s) {
        if (s.timestamp < start_ms_ || s.timestamp > end_ms_) return;
        if (s.timestamp > newest_seen_ts_) newest_seen_ts_ = s.timestamp;
        Entry e{};
        e.ts    = s.timestamp;
        e.seq   = next_arrival_seq_++;
        e.is_l2 = true;
        e.l2    = s;
        InsertBuffered(std::move(e));
      });
      break;
    }

    default:
      break;
  }

  cursor_offset_ = next;
  return true;
}

}  // namespace mokuren::mdf
