#pragma once

#include "live_streamer.hpp"
#include "market_data/writer.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace mokuren {

// UTC day key (whole days since the Unix epoch). Day boundaries are UTC
// midnight, matching the recorder's rotation rule. Shared with the merge tool
// so its window selection cannot drift from the file naming.
constexpr int64_t kNsPerDay = 86'400'000'000'000LL;

int64_t     DayKeyFromNs(int64_t ts_ns);        // ns -> day key
std::string DayStringFromKey(int64_t day_key);  // day key -> "YYYY-MM-DD"

struct RecorderConfig {
  std::string path;

  // Session metadata only; the recorder never uses these to reach a venue.
  std::string venue;
  std::string env;
  std::string category;
  std::string symbol;

  // 0 records every book frame the streamer emits. Above that, at most one
  // frame per interval. Trades are never downsampled: they are cheap, and a
  // missing print changes the footprint while a coarser book mostly does not.
  int64_t l2_min_interval_ms = 0;

  // Run the same checks the live gate runs. A frame the engine would have
  // refused is not written -- the format has no field meaning "this one was
  // junk", so recording it would make it indistinguishable from real data.
  // It is counted instead, so the session summary shows what was lost.
  bool validate = true;
  bool require_monotonic_timestamps = false;
  size_t min_book_depth = 1;

  // Daily mode. `path` becomes a root directory; each session is lifted into
  // <root>/<venue>-<symbol>/<venue>-<symbol>-YYYY-MM-DD.mkn, where the date
  // comes from the first accepted record's ts_exchange_ns. Rotation happens
  // lazily between records on a UTC-midnight crossing -- never inside a
  // record, so every closed file ends on a complete block boundary.
  bool daily = false;

  // Daily mode only. On a name collision (mid-day restart), overwrite the
  // existing file. Off by default: the restart instead gets a T<HHMMSS>
  // suffix and both sessions coexist. Only filenames are affected -- the
  // merger orders by each file's index first_ts_ns, never by name.
  bool overwrite_existing = false;
};

// Records one streamer's output to a .mkn file.
//
// It attaches to an IMarketStreamer rather than creating one: the whole point
// is to capture what the live path already parses, not to run a second, less
// exercised venue client alongside it.
//
// Callbacks are additive, so the engine and the recorder can share a streamer.
// The lifecycle callbacks (Finished/Error) are deliberately NOT taken: they
// belong to whoever owns the stream's lifecycle, and a recorder that stole them
// would silently stop the engine from reacting to a disconnect.
class MarketDataRecorder {
 public:
  struct Stats {
    uint64_t trades_seen = 0;
    uint64_t trades_written = 0;
    uint64_t trades_rejected = 0;
    uint64_t l2_seen = 0;
    uint64_t l2_written = 0;
    uint64_t l2_rejected = 0;
    uint64_t l2_throttled = 0;
    // reconnect sentinels (integrity::IsBookGap). Not errors: a gap marker is
    // the streamer telling us the book is unknown, and there is no frame to
    // record until the next real snapshot arrives.
    uint64_t l2_gaps = 0;
    // Daily mode: number of UTC-midnight cuts performed.
    uint64_t rotations = 0;
  };

  explicit MarketDataRecorder(RecorderConfig cfg);
  ~MarketDataRecorder();

  MarketDataRecorder(const MarketDataRecorder&) = delete;
  MarketDataRecorder& operator=(const MarketDataRecorder&) = delete;

  // Registers the trade and l2 sinks. Does not open the file.
  void Attach(IMarketStreamer& streamer);

  bool Open();

  // Must be called after IMarketStreamer::Stop() returns: Stop() joins the
  // producer thread, and only then is the writer unshared. Returns false if the
  // file could not be finalised.
  bool Close();

  // True from a successful Open() until Close(): in daily mode that includes
  // the window before the first record has created a file.
  bool is_open() const { return opened_; }

  const Stats& stats() const { return stats_; }
  const std::string& last_error() const {
    return error_.empty() ? writer_.last_error() : error_;
  }

 private:
  void OnTrade(const TradeEvent& t);
  void OnL2(const L2Snapshot& s);

  // ensure the writer is attached to the right day's file for ts_ns, rotating
  // across a midnight boundary if needed; false means "drop this record".
  bool EnsureOpenFor(int64_t ts_ns);
  bool RotateIfNeeded(int64_t ts_ns);
  bool OpenDayFor(int64_t ts_ns);
  bool CloseCurrentFile();  // flush + close + .part -> .mkn

  RecorderConfig cfg_;
  mdf::Writer writer_;
  Stats stats_{};
  int64_t last_trade_ts_ = 0;
  int64_t last_book_ts_ = 0;
  int64_t last_written_book_ts_ = 0;

  // Daily-mode state.
  bool        opened_ = false;
  std::string error_;
  std::string dir_;             // <root>/<venue>-<symbol>
  std::string current_path_;    // final name, without .part
  std::string current_part_path_;
  int64_t     current_day_ = -1;
};

}  // namespace mokuren
