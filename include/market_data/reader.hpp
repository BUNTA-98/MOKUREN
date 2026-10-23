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
#include <string>
#include <vector>

namespace mokuren::mdf {

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

}  // namespace mokuren::mdf
