#pragma once

// Reader for the .mkn market-data format (v1).
// Normative specification: docs/market_data_format.md
//
// Unlike writer.hpp, this header depends on the engine's own record types.
// That is deliberate: the reader's output IS the engine's input, so a separate
// "neutral" representation would only add a conversion layer whose job is to
// agree with itself. Bytes on disk are mdf; everything this returns is
// TradeEvent / L2Snapshot in engine units.
//
// Units: the file carries nanoseconds, the engine operates in milliseconds.
// The shift happens here, once, and nowhere else. Timestamps on the returned
// records are milliseconds.
//
// Ordering: both vectors come back sorted by timestamp (stable, so records that
// share a millisecond keep their arrival order). DataManager::SliceTrades and
// SliceL2 stop scanning once they pass `end_time`, which is only correct on an
// ascending vector -- the CSV path happens to be ascending, and this one
// guarantees it.

#include "market_data/format.hpp"
#include "trade_event.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace mokuren::mdf {

class Writer;  // only an opaque reference is needed; see CopyPayloadTo

// Metadata block (JSON) as far as the reader cares. Unknown keys are ignored:
// the block kinds are what the reader dispatches on, so a stale or extended
// metadata string can never make it mis-decode a record.
struct SessionMeta {
  std::string venue;
  std::string env;
  std::string category;
  std::string symbol;
  std::string source;
  uint32_t depth_per_side = 0;
  int64_t created_unix_ns = 0;
  // mdf::kWriterVersion as recorded at write time. Empty for a file written
  // before the field existed, which is itself informative.
  std::string writer_version;

  // false for a recording that was never finalised: the trailer (metadata and
  // index) is written by Close(), so a crash leaves the blocks readable and the
  // session fields unknown. That is a usable file, not a broken one.
  bool has_metadata = false;
};

struct SessionData {
  SessionMeta meta;
  std::vector<TradeEvent> trades;
  std::vector<L2Snapshot> l2;

  uint64_t blocks_read = 0;
  uint64_t records_read = 0;   // raw records in blocks, before any skip
  uint64_t trades_skipped = 0;
  uint64_t l2_skipped = 0;
  // Blocks this reader could not use: a failed payload checksum, an
  // unimplemented codec, or a payload past the end of the recording. Distinct
  // from the per-record skips above because a whole block's worth of data is
  // gone -- and which block it was is not something the surviving records can
  // tell you.
  uint64_t blocks_skipped = 0;
  std::vector<std::string> warnings;

  // Non-empty when reading stopped early (bad magic mid-file, short read).
  // Anything decoded before that point stays in trades/l2: a corrupt tail must
  // not cost the hour of data in front of it. Check `warnings` for recoverable
  // damage that did not stop the read.
  std::string error;

  bool ok() const { return error.empty(); }
  bool empty() const { return trades.empty() && l2.empty(); }
};

// Cheap probe: a regular file whose first bytes are the format magic. One
// 8-byte read, so callers can branch before committing to the CSV pipeline.
bool IsSessionFile(const std::string& path);

SessionData ReadSessionFile(const std::string& path);

// Same decode, restricted to the blocks that overlap [start_ns, end_ns] -- both
// in NANOSECONDS, matching the on-disk blocks and not the milliseconds the
// returned records carry.
//
// Exists because a merged multi-day recording decodes to roughly its own size
// in heap (a book record is 680 B on disk, 688 B in L2Snapshot), so a file
// larger than RAM cannot be read at all through the full decode -- the process
// dies in std::bad_alloc with no diagnosis. Reading one day at a time can.
//
// What it gives up, stated plainly because both are easy to misread:
//   - Block granular, not record granular. A block whose span straddles a bound
//     is decoded whole, so each channel may extend up to one block span (at
//     most Writer::Options::max_block_span_ns, 1s by default) outside the
//     range. Nothing INSIDE the range is missing.
//   - Only the blocks it decodes are checksum-verified. A block outside the
//     range is skipped on the strength of its header, so a corrupt payload
//     there is NOT reported. Use a full read to audit a file; use this to look
//     at one.
//   - A BookDelta block before start_ns is not applied, because applying it
//     would mean decoding from the beginning. The recorder writes snapshots
//     only and the merger refuses deltas, so this is a warning, not an error --
//     but on a delta-carrying file the first book state emitted may be wrong.
SessionData ReadSessionFileRange(const std::string& path, int64_t start_ns,
                                 int64_t end_ns);

// Block-level reader used by the merger.
//
// Unlike ReadSessionFile (which decodes into engine types and applies per-
// record validation), this exposes the raw block stream so a caller can copy
// payloads verbatim into a Writer. No decode, no re-serialisation -- the
// recorder already validated at write time, so a merged file is a
// byte-identical concatenation of its sources' payloads within each window.
class Reader {
 public:
  // Metadata the merger needs, taken from the file's trailer + index. Fields
  // that ReadSessionFile exposes through SessionMeta are mirrored here so the
  // merger does not need to pull in the whole decode path.
  struct Meta {
    std::string venue;
    std::string env;
    std::string category;
    std::string symbol;
    std::string source;
    uint32_t    depth_per_side  = 0;
    int64_t     created_unix_ns = 0;
    // mdf::kWriterVersion as recorded at write time; empty when unrecorded.
    std::string writer_version;

    // From the index after Open(), which is what makes scanning a directory
    // cheap. Overwritten from the block chain by ScanChainBounds() -- and the
    // index is exactly what cannot be trusted when it disagrees with that
    // chain, which is the case index_mismatch_count() reports.
    int64_t  first_ts_ns  = 0;
    int64_t  last_ts_ns   = 0;
    uint64_t record_count = 0;

    // Largest amount, in ns, by which any block starts before the previous
    // block OF THE SAME KIND ends, in physical (block_offset) order. 0 when
    // every stream is strictly ascending. Per kind, because trades and book are
    // flushed independently and interleave: comparing across kinds compares two
    // unrelated clocks.
    //
    // A magnitude, not a bool, because a step backwards is not by itself
    // damage: MarketDataRecorder::RotateIfNeeded keeps a late print rather than
    // dropping it, so every file this recorder writes has small ones. The
    // merger compares this against a tolerated skew instead of rejecting the
    // file outright. The min/max pair above cannot answer it at all, because a
    // file whose blocks are shuffled still has a well-defined min and max.
    int64_t max_inversion_ns = 0;

    // The two timestamps behind max_inversion_ns: the previous block's
    // last_ts_ns and this block's first_ts_ns. Index entries are not covered by
    // any checksum (only the payload is), so when the magnitude is implausible
    // the pair is what separates "a late print" from "an index entry that was
    // never valid": a timestamp in the wrong unit, or one left at zero, is
    // obvious from its value, while a genuine gap shows two real dates.
    int64_t max_inversion_prev_ns = 0;
    int64_t max_inversion_next_ns = 0;

    // Which stream produced it (RecordKind). The magnitude alone cannot be
    // acted on: a stale TRADE is a feed problem -- a resubscribe replaying old
    // prints -- while a stale SNAPSHOT is the local book reconstruction
    // emitting frames out of order. Same number, different fix.
    uint16_t max_inversion_kind = 0;

    // Where the chain starts, and where the index claims the first block
    // starts. Equal on a healthy file. The DELTA is the diagnosis for a
    // disagreement: a constant offset means an older writer stored a different
    // field there (a semantics change no structural check can see, since index
    // entries carry no checksum), while a varying or garbage offset means the
    // file was damaged after it was written. Those have opposite fixes.
    uint64_t first_block_offset = 0;
    uint64_t first_index_offset = 0;

    // True when the file carries a writer trailer (metadata + index present).
    // NOT a guarantee that every byte was written: Writer::Close() patches the
    // trailer even after a failed flush, on purpose, so a recording stays
    // readable up to the damage. Data integrity comes from the per-block crc
    // and bounds checks in CopyPayloadTo, not from this flag.
    bool finalised = false;
  };

  // Reads the header, metadata block and index. Does NOT touch the payload
  // blocks: scanning a day's worth of files pays only for the table of
  // contents, not the data.
  bool Open(const std::string& path);

  bool is_open() const { return in_.is_open(); }
  const std::string& last_error() const { return error_; }
  const Meta& metadata() const { return meta_; }
  uint64_t record_count() const { return meta_.record_count; }

  // Records actually copied by the last CopyPayloadTo, counted from the block
  // headers it walked rather than from the index's record_count. The two agree
  // on a healthy file and disagree on one whose trailer is damaged; the merged
  // file's own metadata comes from the block headers, so this is the number
  // that describes what was written.
  uint64_t copied_records() const { return copied_records_; }

  // Index entries that did not line up with a block found by walking the chain.
  // 0 on a healthy file. Non-zero means the trailer's index is damaged or
  // stale; the payload was still copied correctly, because CopyPayloadTo never
  // trusts the index for what it copies.
  uint64_t index_mismatch_count() const { return index_mismatches_; }

  // Entries Open() parsed out of the trailer, and blocks the chain walk found.
  // Equal on a healthy file. The RATIO is the diagnosis: index_size is fixed by
  // the file, so parsing it at kIndexEntrySize yields |index_size / 32| entries
  // whatever size the writer used. 32 * entries / blocks therefore recovers the
  // writer's own entry size -- 32 means this build's layout, anything else means
  // that build described entries differently and every parsed offset is shifted.
  uint64_t index_entries() const { return index_.size(); }
  uint64_t blocks_walked() const { return blocks_walked_; }

  // Overwrite metadata()'s first_ts_ns / last_ts_ns / record_count and the
  // inversion figures from the BLOCK CHAIN, walking headers and seeking over
  // payloads. Required before anything treats those values as facts rather
  // than as a hint: a caller that orders files by them, or labels an output
  // from them, is relying on values the trailer can get wrong (index entries
  // are not checksummed; only payloads are), and three of four inputs in one
  // real directory were wrong that way.
  //
  // Header-only: 56 bytes per block, no payload read, so it costs about as
  // much as the directory scan it accompanies and a small fraction of the copy
  // that follows.
  //
  // Returns false when the chain itself is unreadable at some offset, with
  // last_error() naming it -- that block would fail CopyPayloadTo too, so
  // failing here reports it before a partial output exists.
  bool ScanChainBounds();

  // Walk the BLOCK CHAIN from header_size to the end of the data -- not the
  // index -- verify each block (magic + bounds + crc + codec), and re-emit it
  // verbatim through Writer::AppendBlock. Returns false on a hard error -- a
  // corrupted source block is a hard error on purpose: skipping it would leave
  // a hole in the merged file that looks exactly like a venue gap.
  //
  // The chain, not the index, because index entries carry no checksum: one
  // damaged entry reads as a corrupt payload ("block magic mismatch" from a
  // file whose data is fine), and an index that lost entries silently copies
  // fewer blocks than the file holds. Every block header is self-describing
  // (magic, kind, stride, count, payload_size, crc), which is exactly what a
  // linear walk needs -- the same thing DecodeBlocks does. The index is then
  // compared against what the walk found, and any disagreement is counted in
  // index_mismatch_count() rather than silently accepted.
  bool CopyPayloadTo(Writer& out);

 private:
  std::ifstream in_;
  FileHeader header_{};
  uint64_t file_size_ = 0;
  // Where the block chain ends: metadata_offset when the trailer is present,
  // otherwise the real end of the file. An unfinalised recording still has a
  // readable prefix, which is the whole reason Reader can open one.
  uint64_t data_end_ = 0;
  Meta meta_{};
  std::vector<IndexEntry> index_;
  uint64_t copied_records_ = 0;
  uint64_t index_mismatches_ = 0;
  uint64_t blocks_walked_ = 0;
  std::string error_;
};

}  // namespace mokuren::mdf
