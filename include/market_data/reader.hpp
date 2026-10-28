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

    // From the index. Zero when the file is unfinalised.
    int64_t  first_ts_ns  = 0;
    int64_t  last_ts_ns   = 0;
    uint64_t record_count = 0;

    // True when the file carries a writer trailer. An unfinalised file has
    // readable blocks in front of the crash but no index, so a merger cannot
    // safely concatenate it: the merged file would silently lose the tail.
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

  // Walk the index in physical order, verify each block (magic + crc +
  // codec), and re-emit it verbatim through Writer::AppendBlock. Returns
  // false on a hard error -- a corrupted source block is a hard error on
  // purpose: skipping it would leave a hole in the merged file that looks
  // exactly like a venue gap.
  bool CopyPayloadTo(Writer& out);

 private:
  std::ifstream in_;
  FileHeader header_{};
  Meta meta_{};
  std::vector<IndexEntry> index_;
  std::string error_;
};

}  // namespace mokuren::mdf
