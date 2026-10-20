#pragma once

// On-disk layout for the .mkn market-data format (v1).
// Normative specification: docs/market_data_format.md
//
// This header describes *bytes on disk only*. Engine-facing views live in
// book_view.hpp. Keep the two separate: the on-disk layout is frozen by
// versioning rules, the engine view is free to change per release.
//
// Units: every ts_* field here is nanoseconds since Unix epoch. The engine
// continues to operate in milliseconds; the reader performs one integer
// shift at the boundary. See spec section 11.

#include <cstddef>
#include <cstdint>

namespace mokuren::mdf {

// --- constants -------------------------------------------------------------

inline constexpr char     kFileMagic[8]     = {'M','O','K','U','R','E','N','\0'};
inline constexpr uint32_t kBlockMagic       = 0x424E4B4Du; // 'M','K','N','B' LE
inline constexpr uint32_t kEndianTagLe      = 0x01020304u;
inline constexpr uint16_t kFormatVersion    = 1;
inline constexpr uint16_t kMinReaderVersion = 1;
inline constexpr uint32_t kFileHeaderSize   = 128;
inline constexpr uint32_t kBlockHeaderSize  = 56;
inline constexpr uint32_t kIndexEntrySize   = 32;

// Compile-time ceiling on per-side depth. A dataset may declare less; it
// may not declare more without recompiling and bumping kFormatVersion.
// A dataset that only publishes top-of-book uses depth 1.
inline constexpr uint32_t kMaxBookDepth = 20;

// --- enums -----------------------------------------------------------------

enum class RecordKind : uint16_t {
  Trade         = 1,
  BookSnapshot  = 2,
  BookDelta     = 3,
  Mark          = 100, // reserved: funding, liquidations, ...
  SessionMarker = 101, // reserved: pause/resume, recording gaps
};

enum class BlockCodec : uint16_t {
  None = 0,
  Zstd = 1,
};

// Who was the *aggressor* of a print. Never store a venue-specific
// "is buyer maker" boolean in the format: it flips meaning per venue and
// silently inverts footprint, delta, POC and every imbalance threshold.
enum class Aggressor : uint8_t {
  Unknown = 0,
  Buy     = 1,
  Sell    = 2,
};

enum class Granularity : uint8_t {
  Tob = 0, // top of book, depth 1 per side
  Mbp = 1, // multi-level by price
  Mbo = 2, // order-by-order (future)
};

enum class BookEncoding : uint8_t {
  Snapshot         = 0, // only BookSnapshot blocks
  SnapshotPlusDelta = 1, // BookSnapshot checkpoints + BookDelta blocks
};

// --- records ---------------------------------------------------------------

struct BookLevel {
  double price;
  double qty;
};
static_assert(sizeof(BookLevel) == 16, "BookLevel must be 16 bytes");

struct FileHeader {
  char     magic[8];           // kFileMagic
  uint16_t format_version;     // kFormatVersion
  uint16_t min_reader_version; // kMinReaderVersion
  uint32_t header_size;        // kFileHeaderSize
  uint32_t endian_tag;         // kEndianTagLe
  uint32_t double_size;        // sizeof(double)
  uint32_t pointer_size;       // sizeof(void*)
  uint32_t reserved0;
  int64_t  created_unix_ns;
  uint64_t metadata_offset;
  uint64_t metadata_size;
  uint64_t index_offset;
  uint64_t index_size;
  uint64_t file_size;
  uint32_t metadata_crc32c;
  uint32_t file_flags;         // bit0 = includes recording gaps
  uint8_t  reserved[40];
};
static_assert(sizeof(FileHeader) == kFileHeaderSize,
              "FileHeader must match kFileHeaderSize");
static_assert(offsetof(FileHeader, magic) == 0,
              "FileHeader.magic must be the first field");

struct BlockHeader {
  uint32_t magic;          // kBlockMagic
  uint16_t kind;           // RecordKind
  uint16_t codec;          // BlockCodec
  uint32_t record_size;    // stride in bytes (uncompressed)
  uint32_t record_count;
  int64_t  first_ts_ns;
  int64_t  last_ts_ns;
  uint64_t payload_offset;
  uint64_t payload_size;   // on-disk bytes (after codec)
  uint32_t payload_crc32c;
  uint32_t reserved;
};
static_assert(sizeof(BlockHeader) == kBlockHeaderSize,
              "BlockHeader must match kBlockHeaderSize");

struct IndexEntry {
  uint64_t block_offset;
  int64_t  first_ts_ns;
  int64_t  last_ts_ns;
  uint32_t record_count;
  uint16_t kind;           // RecordKind
  uint16_t codec;          // BlockCodec
};
static_assert(sizeof(IndexEntry) == kIndexEntrySize,
              "IndexEntry must match kIndexEntrySize");

struct TradeRec {
  int64_t ts_exchange_ns;
  int64_t ts_recv_ns;
  int64_t seq;             // venue trade id, 0 when absent
  double  price;
  double  qty;
  uint8_t aggressor;       // Aggressor
  uint8_t flags;           // see trade_flags
  uint16_t reserved16;
  uint32_t reserved32;
};
static_assert(sizeof(TradeRec) == 48, "TradeRec must be 48 bytes");

struct BookSnapshotRec {
  int64_t  ts_exchange_ns;
  int64_t  ts_recv_ns;
  int64_t  seq;
  uint32_t n_bids;
  uint32_t n_asks;
  uint8_t  flags;          // see snapshot_flags
  uint8_t  reserved[3];
  uint32_t reserved32;
  // Followed by BookLevel levels[n_bids + n_asks]:
  //   bids best-first, then asks best-first.
  // Block stride = sizeof(BookSnapshotRec)
  //              + (max_n_bids_in_block + max_n_asks_in_block) * 16.
  // Unused level slots are zero-filled and ignored by readers.
  // n_bids and n_asks must be <= kMaxBookDepth.
};
static_assert(sizeof(BookSnapshotRec) == 40, "BookSnapshotRec must be 40 bytes");

struct BookChange {
  uint8_t  side;           // see book_change_side
  uint8_t  action;         // see book_change_action
  uint16_t reserved16;
  uint32_t reserved32;
  double   price;
  double   qty;            // ignored when action == kRemove
};
static_assert(sizeof(BookChange) == 24, "BookChange must be 24 bytes");

struct BookDeltaRec {
  int64_t  ts_exchange_ns;
  int64_t  ts_recv_ns;
  int64_t  seq;
  uint16_t n_changes;
  uint8_t  flags;
  uint8_t  reserved8;
  uint32_t reserved32;
  // Followed by BookChange changes[n_changes].
};
static_assert(sizeof(BookDeltaRec) == 32, "BookDeltaRec must be 32 bytes");

// --- flag bits -------------------------------------------------------------

namespace snapshot_flags {
inline constexpr uint8_t kResync    = 1u << 0;
inline constexpr uint8_t kTruncated = 1u << 1;
inline constexpr uint8_t kStale     = 1u << 2;
} // namespace snapshot_flags

namespace trade_flags {
inline constexpr uint8_t kLiquidation = 1u << 0;
inline constexpr uint8_t kBlockTrade  = 1u << 1;
} // namespace trade_flags

namespace book_change_side {
inline constexpr uint8_t kBid = 0;
inline constexpr uint8_t kAsk = 1;
} // namespace book_change_side

namespace book_change_action {
inline constexpr uint8_t kSet    = 0;
inline constexpr uint8_t kRemove = 1;
} // namespace book_change_action

} // namespace mokuren::mdf
