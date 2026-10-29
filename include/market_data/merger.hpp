#pragma once

#include "market_data/writer.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mokuren::mdf {

// Window granularity for the merger. Both are aligned to UTC epoch days so the
// window a record lands in is a pure function of its ts_exchange_ns -- the same
// rule the recorder uses to name daily files.
enum class WindowKind { Weekly, Monthly };

// One input file discovered by the merger. `first_ts_ns` is the index's
// first_ts_ns, which is what the merger orders by -- never the filename, so the
// recorder's T<HHMMSS> restart suffix is harmless.
struct InputFile {
  std::string path;
  int64_t     first_ts_ns = 0;
  int64_t     last_ts_ns  = 0;
};

struct MergerConfig {
  // Directory to scan. Non-recursive: the recorder writes one flat directory
  // per instrument, and the merger mirrors that.
  std::string input_dir;

  // Sibling root for output, e.g. "<root>/weekly". The merger creates
  // <output_root>/<venue>-<symbol>/ and writes there.
  std::string output_root;

  WindowKind kind = WindowKind::Weekly;

  // Instrument the merged files belong to. Used to build the output filename
  // and to fill each output's session header. Every input file's metadata must
  // match -- a directory holding two instruments is a misconfiguration, not
  // something to merge silently.
  std::string venue;
  std::string symbol;

  // Overwrite an existing output file. Off by default: a re-run should not
  // silently clobber a previous merge.
  bool overwrite_existing = false;
};

struct MergerStats {
  uint64_t files_scanned   = 0;
  uint64_t files_skipped   = 0;  // *.part, non-finalised, unreadable
  uint64_t files_merged    = 0;
  uint64_t windows_written = 0;
  uint64_t records_copied  = 0;
};

// Merges a directory of daily .mkn files into weekly or monthly files.
//
// Payload blocks are copied verbatim -- records are never re-serialised, so a
// merged file is byte-identical to the concatenation of its sources' payloads
// within each window. The seam is safe because the recorder only ever writes
// full BookSnapshot records and never BookDelta, so no book state crosses a
// block boundary: a reader rebuilding the merged book from the first snapshot
// it sees ends up in the same state it would have in the daily source.
class Merger {
 public:
  explicit Merger(MergerConfig cfg);

  // Scans, groups by window, writes each window as
  // <output_root>/<venue>-<symbol>/<venue>-<symbol>-<label>.mkn.part, then
  // renames to .mkn on success. <label> is WindowLabelFromKey: the first day
  // of the week window ("YYYY-MM-DD") for Weekly, "YYYY-MM" for Monthly.
  // Returns false on the first hard error; per-file skips are counted, not
  // fatal.
  bool Run();

  const MergerStats& stats() const { return stats_; }
  const std::string& last_error() const { return error_; }

 private:
  bool ScanInputs(std::vector<InputFile>& out);
  bool MergeWindow(const std::vector<InputFile>& files, int64_t window_key);

  MergerConfig cfg_;
  MergerStats  stats_{};
  std::string  error_;
};

// Window key for a timestamp: day_key/7 for weekly, YYYY-MM for monthly.
// Exposed so tests and callers can predict which file a record lands in.
int64_t     WindowKeyFromNs(int64_t ts_ns, WindowKind kind);
// Empty string if window_key is outside [197001, 999912] (monthly) or if the
// weekly date could not be formatted. Callers must treat "" as a hard error.
std::string WindowLabelFromKey(int64_t window_key, WindowKind kind);

}  // namespace mokuren::mdf
