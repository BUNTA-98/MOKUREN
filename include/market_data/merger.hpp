#pragma once

#include "market_data/writer.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mokuren::mdf {

// Window granularity for the merger. Weekly and Monthly are aligned to UTC
// epoch days so the window a record lands in is a pure function of its
// ts_exchange_ns -- the same rule the recorder uses to name daily files.
//
// All is the no-bucket mode: the whole input directory becomes ONE output file,
// spanning whatever it spans. Nothing about the calendar decides the cut --
// only the data does. See Merger::Run for the preconditions it enforces.
enum class WindowKind { Weekly, Monthly, All };

// One input file discovered by the merger. The bounds come from the file's
// BLOCK CHAIN, not from its trailer index -- see Reader::ScanChainBounds. A
// trailer can disagree with its payload (three of four inputs in one real
// directory did), and these two values order the concatenation and label the
// output, so taking them from a damaged trailer would misorder the merge and
// name the file after a span it does not cover while the bytes stayed correct.
// Never the filename either, so the recorder's T<HHMMSS> restart suffix is
// harmless.
struct InputFile {
  std::string path;
  int64_t     first_ts_ns = 0;
  int64_t     last_ts_ns  = 0;
  // Largest step backwards within one record stream, chain-derived via
  // Reader::ScanChainBounds. Not a bool, and not taken from the trailer index:
  // index entries carry no checksum, so a damaged one reports an inversion the
  // payloads do not contain -- which is what made one file here appear to hold
  // a 53-hour-stale print. Measured from the blocks, that value is zero. The
  // check stays because the recorder does keep a late print on purpose, and a
  // genuine one would show up as a small value rather than none.
  // Compared against MergerConfig::max_skew_ns.
  int64_t     max_inversion_ns = 0;
  // The two timestamps behind it, carried through so a failure names them
  // instead of just a magnitude. Chain-derived, so they describe two blocks
  // that really exist at those times.
  int64_t     max_inversion_prev_ns = 0;
  int64_t     max_inversion_next_ns = 0;
  // Which stream produced it (RecordKind). Carried so the refusal can say
  // "trade" or "book snapshot": those are different bugs with the same size.
  uint16_t    max_inversion_kind = 0;
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

  // WindowKind::All only. When true (the default) the merge is refused unless
  // every input's streams and the seams between inputs stay within
  // max_skew_ns of ascending. Both quantities are chain-derived, so a damaged
  // trailer cannot trip this -- it could, and did, while the reader measured
  // them from the index.
  bool verify_timeline = true;

  // How far a timeline may step backwards before it is treated as damaged
  // rather than late. Applied to both comparisons, because they are the same
  // phenomenon at two scales: the recorder keeps a late print, so a file's own
  // blocks can step back by the lateness, and a late print from the previous
  // day lands in the newer file, so the seam between files overlaps by it too.
  //
  // 60s is deliberately far above anything a venue sends late (sub-second in
  // practice) and far below a re-recorded span, so a genuine duplicate — a
  // reconnect replaying minutes of prints, or the same day recorded twice —
  // still fails instead of doubling the tape. Lower it to tighten, raise it or
  // pass --no-verify to accept whatever is there.
  int64_t max_skew_ns = 60'000'000'000LL;
};

struct MergerStats {
  uint64_t files_scanned   = 0;
  uint64_t files_skipped   = 0;  // *.part, non-finalised, unreadable
  uint64_t files_merged    = 0;
  uint64_t windows_written = 0;
  uint64_t records_copied  = 0;

  // Inputs whose trailer index disagreed with their block chain. The copy is
  // unaffected -- CopyPayloadTo walks the blocks, never the index -- so this is
  // reported rather than fatal. It is not cosmetic, though: a damaged index
  // means that source's own first/last timestamps and record counts were
  // wrong, so the merged file's span label and totals can differ from what the
  // daily file claimed, and the file is worth re-recording or inspecting.
  uint64_t files_index_repair = 0;

  // Full paths this run wrote, in order. The label is derived from the block
  // chain, and an input's own start/end can be dragged by a stale record, so
  // the output name is NOT predictable from the arguments -- printing it is
  // what lets a caller verify an output instead of looking for the name it
  // assumed.
  std::vector<std::string> outputs;
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
  //
  // A file is bucketed by the window its span ENDS in, and inputs are
  // concatenated in that same order. Both ends come from the block chain, so a
  // damaged trailer can move neither. Ordering by the end rather than the start
  // is still the safer rule -- a late record can only pull a start backwards,
  // never an end forwards -- and it is what keeps a merge sane with
  // verify_timeline off; with it on, a start pulled far enough back to reorder
  // its file fails that check first.
  //
  // WindowKind::All writes exactly one file, labelled by the span it covers
  // ("<first-day>_<last-day>", or a single day when both ends coincide). No
  // calendar boundary is consulted. In that mode per-file skips are NOT
  // tolerated: a skipped input is a hole in the single output, so Run fails
  // instead of emitting a file that quietly drops a span. An empty directory is
  // an error there too, not a clean no-op.
  //
  // Returns false on the first hard error; per-file skips are counted and not
  // fatal in the bucketed modes.
  bool Run();

  const MergerStats& stats() const { return stats_; }
  const std::string& last_error() const { return error_; }

 private:
  bool ScanInputs(std::vector<InputFile>& out);
  // WindowKind::All precondition, on chain-derived bounds: every input's
  // streams within max_skew_ns of ascending, and no two inputs overlapping by
  // more than max_skew_ns in the order ScanInputs established (by last_ts_ns).
  // Sets error_ and returns false on the first violation -- so it is not const,
  // even though it reads nothing but its argument.
  bool VerifyTimeline(const std::vector<InputFile>& files);
  bool MergeWindow(const std::vector<InputFile>& files, int64_t window_key);

  MergerConfig cfg_;
  MergerStats  stats_{};
  std::string  error_;
};

// Window key for a timestamp: day_key/7 for weekly, YYYY-MM for monthly, and
// always 0 for All (which has no key -- Run passes 0 and lets MergeWindow name
// the file from the span).
// Exposed so tests and callers can predict which file a record lands in.
int64_t     WindowKeyFromNs(int64_t ts_ns, WindowKind kind);
// Empty string if window_key is outside [197001, 999912] (monthly) or if the
// weekly date could not be formatted, and always for All: MergeWindow labels
// that mode from the data, so reaching here with All is a caller bug and ""
// makes it fail instead of inventing a name. Callers must treat "" as a hard
// error.
std::string WindowLabelFromKey(int64_t window_key, WindowKind kind);

}  // namespace mokuren::mdf
