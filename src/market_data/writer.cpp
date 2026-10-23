#include "market_data/writer.hpp"

#include "market_data/crc32c.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>

namespace mokuren::mdf {
namespace {

int64_t NowNs() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

Writer::~Writer() {
  // A recording left open by an exception path is still a valid prefix of a
  // file once the header is patched, so try to finalise rather than leak the
  // handle. Close() is idempotent, so an explicit Close() before destruction
  // costs nothing.
  Close();
}

bool Writer::Open(const std::string& path, const SessionInfo& info,
                  const Options& opts) {
  if (file_) {
    Fail("writer already open");
    return false;
  }

  // The on-disk layout is little-endian by definition. Storing host-order
  // doubles on a big-endian box would produce a file that silently reads as
  // garbage, so refuse instead.
  const uint16_t probe = 1;
  if (*reinterpret_cast<const uint8_t*>(&probe) != 1) {
    Fail("big-endian host unsupported: the .mkn format is little-endian");
    return false;
  }

  if (opts.max_levels_per_side == 0 ||
      opts.max_levels_per_side > kMaxBookDepth) {
    Fail("max_levels_per_side must be in [1, kMaxBookDepth]");
    return false;
  }
  if (opts.max_changes_per_delta == 0) {
    Fail("max_changes_per_delta must be > 0");
    return false;
  }

  opts_ = opts;
  info_ = info;

  file_ = std::fopen(path.c_str(), "wb");
  if (!file_) {
    Fail("cannot open '" + path + "' for writing");
    return false;
  }

  header_ = FileHeader{};
  std::memcpy(header_.magic, kFileMagic, sizeof(header_.magic));
  header_.format_version = kFormatVersion;
  header_.min_reader_version = kMinReaderVersion;
  header_.header_size = kFileHeaderSize;
  header_.endian_tag = kEndianTagLe;
  header_.double_size = static_cast<uint32_t>(sizeof(double));
  header_.pointer_size = static_cast<uint32_t>(sizeof(void*));
  // bit0 = includes recording gaps. Nothing writes gap markers yet, so the
  // file must not claim to: a reader that trusts the flag would look for
  // SessionMarker blocks that are not there.
  header_.file_flags = 0;
  header_.created_unix_ns = NowNs();

  if (std::fwrite(&header_, sizeof(header_), 1, file_) != 1) {
    Fail("short file header write");
    std::fclose(file_);
    file_ = nullptr;
    return false;
  }
  return true;
}

uint8_t* Writer::Reserve(PendingBlock& block, RecordKind kind, uint32_t stride,
                         int64_t ts) {
  // A block holds one record layout. Mixing a snapshot and a delta in the same
  // block would make record_size meaningless, so a layout change flushes first
  // and the ordering of that kind of stream survives the forced split.
  if (!block.empty() && (block.kind != kind || block.stride != stride)) {
    if (!Flush(block)) return nullptr;
  }
  if (block.empty()) {
    block.kind = kind;
    block.stride = stride;
  }
  const size_t offset = block.payload.size();
  block.payload.resize(offset + stride, 0);
  if (block.count == 0) block.first_ts = ts;
  block.last_ts = ts;
  ++block.count;
  ++records_written_;
  return block.payload.data() + offset;
}

bool Writer::WriteTrade(const TradeRec& rec) {
  if (!file_) {
    Fail("write before open");
    return false;
  }
  uint8_t* slot = Reserve(trades_, RecordKind::Trade, sizeof(TradeRec),
                          rec.ts_exchange_ns);
  if (!slot) return false;
  std::memcpy(slot, &rec, sizeof(rec));
  return MaybeFlush(trades_);
}

bool Writer::WriteSnapshot(const BookSnapshotRec& rec, const BookLevel* levels,
                           size_t n_levels) {
  if (!file_) {
    Fail("write before open");
    return false;
  }
  const size_t cap = static_cast<size_t>(opts_.max_levels_per_side) * 2;
  if (n_levels > cap) {
    Fail("snapshot carries more levels than max_levels_per_side allows");
    return false;
  }
  if (static_cast<size_t>(rec.n_bids) + rec.n_asks != n_levels) {
    Fail("snapshot n_bids + n_asks does not match the level array");
    return false;
  }
  if (rec.n_bids > opts_.max_levels_per_side ||
      rec.n_asks > opts_.max_levels_per_side) {
    Fail("snapshot side depth exceeds max_levels_per_side");
    return false;
  }
  if (n_levels > 0 && levels == nullptr) {
    Fail("snapshot level pointer is null");
    return false;
  }

  const uint32_t stride = static_cast<uint32_t>(
      sizeof(BookSnapshotRec) + cap * sizeof(BookLevel));
  uint8_t* slot = Reserve(book_, RecordKind::BookSnapshot, stride,
                          rec.ts_exchange_ns);
  if (!slot) return false;

  std::memcpy(slot, &rec, sizeof(rec));
  if (n_levels > 0) {
    std::memcpy(slot + sizeof(rec), levels, n_levels * sizeof(BookLevel));
  }
  return MaybeFlush(book_);
}

bool Writer::WriteDelta(const BookDeltaRec& rec, const BookChange* changes) {
  if (!file_) {
    Fail("write before open");
    return false;
  }
  if (rec.n_changes > opts_.max_changes_per_delta) {
    Fail("delta carries more changes than max_changes_per_delta allows");
    return false;
  }
  if (rec.n_changes > 0 && changes == nullptr) {
    Fail("delta change pointer is null");
    return false;
  }

  const uint32_t stride = static_cast<uint32_t>(
      sizeof(BookDeltaRec) +
      static_cast<size_t>(opts_.max_changes_per_delta) * sizeof(BookChange));
  uint8_t* slot = Reserve(book_, RecordKind::BookDelta, stride,
                          rec.ts_exchange_ns);
  if (!slot) return false;

  std::memcpy(slot, &rec, sizeof(rec));
  if (rec.n_changes > 0) {
    std::memcpy(slot + sizeof(rec), changes,
                static_cast<size_t>(rec.n_changes) * sizeof(BookChange));
  }
  return MaybeFlush(book_);
}

bool Writer::MaybeFlush(PendingBlock& block) {
  if (block.empty()) return true;
  if (block.count >= opts_.max_records_per_block ||
      (block.last_ts - block.first_ts) >= opts_.max_block_span_ns) {
    return Flush(block);
  }
  return true;
}

bool Writer::Flush(PendingBlock& block) {
  if (block.empty()) return true;

  const off_t offset = ftello(file_);
  if (offset < 0) {
    Fail("ftello failed");
    return false;
  }

  BlockHeader h{};
  h.magic = kBlockMagic;
  h.kind = static_cast<uint16_t>(block.kind);
  h.codec = static_cast<uint16_t>(BlockCodec::None);
  h.record_size = block.stride;
  h.record_count = block.count;
  h.first_ts_ns = block.first_ts;
  h.last_ts_ns = block.last_ts;
  h.payload_offset = static_cast<uint64_t>(offset) + kBlockHeaderSize;
  h.payload_size = block.payload.size();
  h.payload_crc32c = Crc32c(block.payload.data(), block.payload.size());

  if (std::fwrite(&h, sizeof(h), 1, file_) != 1) {
    Fail("short block header write");
    return false;
  }
  if (!block.payload.empty() &&
      std::fwrite(block.payload.data(), 1, block.payload.size(), file_) !=
          block.payload.size()) {
    Fail("short block payload write");
    return false;
  }

  index_.push_back(IndexEntry{static_cast<uint64_t>(offset), block.first_ts,
                              block.last_ts, block.count, h.kind, h.codec});
  block.Reset();
  return true;
}

bool Writer::FlushAll() {
  // Trades first, then book: matches the order the reader rebuilds a session
  // in, and keeps the index sorted by kind rather than by whatever the last
  // callback happened to be.
  bool ok = Flush(trades_);
  if (!Flush(book_)) ok = false;
  return ok;
}

bool Writer::Close() {
  if (!file_) return true;

  bool ok = FlushAll();

  // Metadata, then index: FileHeader lists them in that order, so a reader that
  // trusts field order instead of seeking still lands on valid bytes.
  const off_t metadata_offset = ftello(file_);
  const std::string metadata = BuildMetadata();
  if (std::fwrite(metadata.data(), 1, metadata.size(), file_) !=
      metadata.size()) {
    Fail("short metadata write");
    ok = false;
  }

  const off_t index_offset = ftello(file_);
  if (!index_.empty() &&
      std::fwrite(index_.data(), sizeof(IndexEntry), index_.size(), file_) !=
          index_.size()) {
    Fail("short index write");
    ok = false;
  }

  const off_t end = ftello(file_);

  header_.metadata_offset = static_cast<uint64_t>(metadata_offset);
  header_.metadata_size = metadata.size();
  header_.metadata_crc32c =
      Crc32c(reinterpret_cast<const uint8_t*>(metadata.data()),
             metadata.size());
  header_.index_offset = static_cast<uint64_t>(index_offset);
  header_.index_size = index_.size() * kIndexEntrySize;
  header_.file_size = static_cast<uint64_t>(end);

  if (fseeko(file_, 0, SEEK_SET) != 0) {
    Fail("fseeko failed while patching the file header");
    ok = false;
  } else if (std::fwrite(&header_, sizeof(header_), 1, file_) != 1) {
    Fail("short file header rewrite");
    ok = false;
  }

  if (std::fclose(file_) != 0) {
    Fail("fclose failed: the recording may be truncated");
    ok = false;
  }
  file_ = nullptr;
  return ok;
}

std::string Writer::BuildMetadata() const {
  // Shape is provisional until docs/market_data_format.md lands: the fields a
  // reader needs to reconstruct the session are all here, and a reader that
  // only looks at the ones it knows will ignore the rest.
  nlohmann::json j;
  j["venue"] = info_.venue;
  j["env"] = info_.env;
  j["category"] = info_.category;
  j["symbol"] = info_.symbol;
  j["source"] = info_.source;
  j["created_unix_ns"] = header_.created_unix_ns;
  j["granularity"] = "mbp";
  j["book_encoding"] = "snapshot";
  j["depth_per_side"] = opts_.max_levels_per_side;
  j["record_sizes"] = {
      {"trade", sizeof(TradeRec)},
      {"book_snapshot", sizeof(BookSnapshotRec) +
                            static_cast<size_t>(opts_.max_levels_per_side) *
                                2 * sizeof(BookLevel)},
      {"book_delta", sizeof(BookDeltaRec) +
                         static_cast<size_t>(opts_.max_changes_per_delta) *
                             sizeof(BookChange)},
  };
  j["blocks"] = index_.size();
  j["records"] = records_written_;
  return j.dump(2);
}

void Writer::Fail(const std::string& what) {
  // Keep the first failure: a cascade of follow-on errors after a short write
  // hides the one that matters.
  if (error_.empty()) error_ = what;
}

}  // namespace mokuren::mdf
