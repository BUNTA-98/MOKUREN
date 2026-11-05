#include "market_data/merger.hpp"

#include "market_data/reader.hpp"
#include "recorder.hpp"  // DayKeyFromNs / DayStringFromKey: single source of truth

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>

namespace mokuren::mdf {

namespace fs = std::filesystem;

namespace {

// Only the kinds the inversion check can produce are named; anything else is a
// reserved kind the reader skips, which cannot reach here.
const char* KindName(uint16_t kind) {
  switch (static_cast<RecordKind>(kind)) {
    case RecordKind::Trade:        return "trade";
    case RecordKind::BookSnapshot: return "book snapshot";
    case RecordKind::BookDelta:    return "book delta";
    default:                       return "unknown";
  }
}

}  // namespace

int64_t WindowKeyFromNs(int64_t ts_ns, WindowKind kind) {
  // All is not bucketed: Run() hands the whole directory to MergeWindow and
  // passes 0, so return the same here rather than a value a caller could
  // mistake for a real window.
  if (kind == WindowKind::All) return 0;
  const int64_t day = DayKeyFromNs(ts_ns);
  if (kind == WindowKind::Weekly) return day / 7;
  // Monthly: derive from the same day string the recorder uses for filenames,
  // so a window can never disagree with the file it came from.
  const std::string date = DayStringFromKey(day);  // "YYYY-MM-DD"
  // "" here means gmtime/strftime failed, which needs a day key this codebase
  // cannot produce -- but std::stoll would throw and Run() has no catch.
  // -1 is rejected by WindowLabelFromKey -> MergeWindow, same path as any
  // other invalid key.
  if (date.size() != 10) return -1;
  return std::stoll(date.substr(0, 4)) * 100 + std::stoll(date.substr(5, 2));
}

std::string WindowLabelFromKey(int64_t window_key, WindowKind kind) {
  // All has no bucket label: MergeWindow names the file after the span it
  // covers. "" is the documented hard-error signal, so a caller that reaches
  // here in All mode fails rather than inventing a name.
  if (kind == WindowKind::All) return "";
  if (kind == WindowKind::Weekly) {
    // A negative key is not a date this codebase can produce; DayStringFromKey
    // would format it as a plausible pre-epoch date rather than reject it.
    // Match the monthly branch's lower bound.
    if (window_key < 0) return "";
    // First day of the week window, as a date.
    return DayStringFromKey(window_key * 7);
  }
  const int64_t year  = window_key / 100;
  const int64_t month = window_key % 100;
  // Out of range means the key did not come from WindowKeyFromNs. Return ""
  // rather than a clamped or negative label: a wrong-but-plausible month
  // names an output file after data it does not contain.
  if (year < 1970 || year > 9999 || month < 1 || month > 12) return "";
  // 4 + 1 + 2 + NUL is 8 exactly; 16 leaves headroom. int, not int64_t, so
  // GCC cannot assume an 18-digit field and warn about a buffer it can see fits.
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04d-%02d",
                static_cast<int>(year), static_cast<int>(month));
  return buf;
}

Merger::Merger(MergerConfig cfg) : cfg_(std::move(cfg)) {}

bool Merger::ScanInputs(std::vector<InputFile>& out) {
  std::error_code ec;
  if (!fs::is_directory(cfg_.input_dir, ec)) {
    error_ = "input_dir is not a directory: " + cfg_.input_dir;
    return false;
  }

  // Hoist the iterator: if construction fails (permission, deleted between
  // is_directory and here), ec is set and the iterator equals end(), so a
  // range-for would silently iterate zero entries and report a clean no-op
  // run. Test ec before the loop instead.
  fs::directory_iterator it(cfg_.input_dir, ec);
  if (ec) {
    error_ = "open input_dir " + cfg_.input_dir + ": " + ec.message();
    return false;
  }
  for (const auto& entry : it) {
    if (!entry.is_regular_file()) continue;
    const std::string path = entry.path().string();

    // *.part is a recorder that did not finish. Never repaired here: promoting
    // it would hide a crash. Reported and skipped.
    if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".part") == 0) {
      ++stats_.files_skipped;
      continue;
    }
    if (path.size() < 4 || path.compare(path.size() - 4, 4, ".mkn") != 0) {
      continue;  // not ours
    }

    ++stats_.files_scanned;

    // Read the index to get first_ts_ns / last_ts_ns. A file whose metadata is
    // not finalised is a hard skip, same reasoning as *.part.
    Reader reader;
    if (!reader.Open(path)) {
      ++stats_.files_skipped;
      continue;
    }
    if (!reader.metadata().finalised) {
      ++stats_.files_skipped;
      continue;
    }
    // Bounds from the block chain, not the trailer's index. This pass is what
    // SORTS the inputs and what LABELS the merged file, and a damaged index --
    // three of four inputs in one real directory -- got both wrong while the
    // payload stayed correct: the merge concatenated the days in index order
    // and named the output after a span taken from the same bad numbers.
    // Header-only, so the cost is 56 bytes per block.
    //
    // A chain that cannot be walked is a hard error rather than a skip: that
    // exact block would fail CopyPayloadTo later, and failing here reports it
    // before a partial output exists.
    if (!reader.ScanChainBounds()) {
      error_ = reader.last_error();
      return false;
    }
    const auto& meta = reader.metadata();
    if (meta.record_count == 0) {
      ++stats_.files_skipped;  // header-only or empty recording
      continue;
    }
    InputFile f;
    f.path        = path;
    f.first_ts_ns      = meta.first_ts_ns;
    f.last_ts_ns       = meta.last_ts_ns;
    f.max_inversion_ns      = meta.max_inversion_ns;
    f.max_inversion_prev_ns = meta.max_inversion_prev_ns;
    f.max_inversion_next_ns = meta.max_inversion_next_ns;
    f.max_inversion_kind    = meta.max_inversion_kind;
    out.push_back(std::move(f));
  }

  // Order by the END of each file's span, not its start -- and never by
  // filename.
  //
  // Sorting by first_ts_ns is the obvious rule and it is wrong here. A block's
  // first_ts_ns is the minimum over the records in it, and the recorder keeps a
  // late print rather than dropping it, so one record stamped two days early
  // drags its file's start back with it: a 2026-10-31 file holding a print from
  // 2026-10-29 05:22 sorted as if it began on 10-29 and was concatenated
  // BETWEEN 10-29 and 10-30, putting two days of that file's payload in the
  // wrong place while every byte stayed correct. The end of the span cannot be
  // dragged backwards that way, and for an ordinary daily file -- start and end
  // on the same day -- this produces exactly the order sorting by the start did.
  //
  // first_ts_ns is the tie-break so two files with the same end stay in a
  // deterministic order.
  std::sort(out.begin(), out.end(),
            [](const InputFile& a, const InputFile& b) {
              if (a.last_ts_ns != b.last_ts_ns) {
                return a.last_ts_ns < b.last_ts_ns;
              }
              return a.first_ts_ns < b.first_ts_ns;
            });
  return true;
}

bool Merger::VerifyTimeline(const std::vector<InputFile>& files) {
  // Two independent claims, both required for "the concatenation is
  // ascending".
  //
  // 1. Within one file, per record stream. A step backwards here is how the
  //    recorder stores a late print -- RotateIfNeeded keeps the record rather
  //    than dropping it, and the block it lands in carries the older stamp. So
  //    the question is not "did it step back" but "by how much". Copying a file
  //    verbatim reproduces its inversions either way; ReadSessionFile
  //    stable-sorts both channels, so a small one costs nothing.
  for (const auto& f : files) {
    if (f.max_inversion_ns > cfg_.max_skew_ns) {
      // Both ends, because the size alone cannot be acted on. A next_ts near 0,
      // or near 1e15 where ns belong, is a bad index entry (index entries are
      // not checksummed; only payloads are). Two real dates mean the file
      // itself spans a gap and the merge would reproduce it -- which the source
      // already has, so --no-verify is a legitimate answer there.
      error_ = f.path + " steps back " +
               std::to_string(f.max_inversion_ns / 1'000'000) +
               " ms in its " + KindName(f.max_inversion_kind) +
               " stream (limit " +
               std::to_string(cfg_.max_skew_ns / 1'000'000) +
               " ms); previous block ends at " +
               std::to_string(f.max_inversion_prev_ns) + " ns (" +
               DayStringFromKey(DayKeyFromNs(f.max_inversion_prev_ns)) +
               "), next block starts at " +
               std::to_string(f.max_inversion_next_ns) + " ns (" +
               DayStringFromKey(DayKeyFromNs(f.max_inversion_next_ns)) +
               "); refusing to merge (--max-skew-seconds or --no-verify "
               "overrides)";
      return false;
    }
  }
  // 2. Between files, in the order ScanInputs sorted them by first_ts_ns. The
  //    same late print explains the seam: a record stamped in the previous day
  //    arrives after the rotation, and because RotateIfNeeded only rotates
  //    forward (`day <= current_day_` keeps the current file) it is written
  //    into the NEWER file. A small overlap is therefore normal; a large one
  //    means a span was recorded twice. Both ends are reported -- which file is
  //    the duplicate is not something the ts values alone can tell you.
  for (size_t i = 1; i < files.size(); ++i) {
    const int64_t overlap = files[i - 1].last_ts_ns - files[i].first_ts_ns;
    if (overlap > cfg_.max_skew_ns) {
      error_ = "inputs overlap by " + std::to_string(overlap / 1'000'000) +
               " ms (limit " + std::to_string(cfg_.max_skew_ns / 1'000'000) +
               " ms): " + files[i - 1].path + " ends at " +
               std::to_string(files[i - 1].last_ts_ns) + ", " + files[i].path +
               " starts at " + std::to_string(files[i].first_ts_ns);
      return false;
    }
  }
  return true;
}

bool Merger::MergeWindow(const std::vector<InputFile>& files,
                         int64_t window_key) {
  // Bucketed modes name the file after the window. All names it after the span
  // it actually covers, so the name cannot claim a boundary the data does not
  // have -- and can never collide with a weekly label.
  std::string label;
  if (cfg_.kind == WindowKind::All) {
    // Both ends are chain-derived (ScanInputs ran ScanChainBounds), so the
    // name describes the records actually in the file rather than what a
    // trailer claimed. Taken across ALL files rather than from front()/back():
    // those are the first file's start and the last file's end, and one stale
    // record makes them understate the span the merged file covers.
    int64_t span_first = files.front().first_ts_ns;
    int64_t span_last  = files.front().last_ts_ns;
    for (const auto& f : files) {
      span_first = std::min(span_first, f.first_ts_ns);
      span_last  = std::max(span_last, f.last_ts_ns);
    }
    const std::string first =
        DayStringFromKey(DayKeyFromNs(span_first));
    const std::string last =
        DayStringFromKey(DayKeyFromNs(span_last));
    if (first.empty() || last.empty()) {
      error_ = "cannot label merged range: unformattable day key";
      return false;
    }
    label = (first == last) ? first : first + "_" + last;
  } else {
    label = WindowLabelFromKey(window_key, cfg_.kind);
  }
  if (label.empty()) {
    error_ = "invalid window key: " + std::to_string(window_key);
    return false;
  }
  const std::string sub   = cfg_.venue + "-" + cfg_.symbol;
  const std::string dir   = (fs::path(cfg_.output_root) / sub).string();

  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    error_ = "create_directories " + dir + ": " + ec.message();
    return false;
  }

  const std::string base =
      cfg_.venue + "-" + cfg_.symbol + "-" + label + ".mkn";
  const std::string final_path = dir + "/" + base;
  const std::string part_path  = final_path + ".part";

  // Check the *.part name too: Writer::Open truncates unconditionally, and a
  // crashed previous run leaves exactly that file. Same rule the recorder uses
  // in OpenDayFor.
  if (!cfg_.overwrite_existing &&
      (fs::exists(final_path, ec) || fs::exists(part_path, ec))) {
    error_ = "output exists (set overwrite_existing): " + final_path;
    return false;
  }

  SessionInfo info;
  info.venue    = cfg_.venue;
  info.env      = "";
  info.category = "";
  info.symbol   = cfg_.symbol;
  info.source   = "merged";

  Writer writer;
  if (!writer.Open(part_path, info)) {
    error_ = "open " + part_path + ": " + writer.last_error();
    return false;
  }

  for (const auto& f : files) {
    Reader reader;
    if (!reader.Open(f.path)) {
      error_ = "open " + f.path + ": " + reader.last_error();
      writer.Close();
      fs::remove(part_path, ec);
      return false;
    }
    // Every input must belong to the same instrument. A directory holding two
    // symbols is a misconfiguration -- merging them would label the output as
    // one and silently mix the data.
    if (reader.metadata().venue  != cfg_.venue ||
        reader.metadata().symbol != cfg_.symbol) {
      error_ = f.path + " is " + reader.metadata().venue + "/" +
               reader.metadata().symbol + ", expected " + cfg_.venue + "/" +
               cfg_.symbol;
      writer.Close();
      fs::remove(part_path, ec);
      return false;
    }
    // Copy payload blocks verbatim; never re-serialise records.
    if (!reader.CopyPayloadTo(writer)) {
      error_ = "copy " + f.path + ": " + reader.last_error();
      writer.Close();
      fs::remove(part_path, ec);
      return false;
    }
    // copied_records(), not record_count(): the latter is summed from the
    // trailer's index, and an index that disagrees with the chain is the exact
    // defect CopyPayloadTo works around. Reporting the index's number would
    // describe a file other than the one that was written.
    stats_.records_copied += reader.copied_records();
    if (reader.index_mismatch_count() > 0) ++stats_.files_index_repair;
    ++stats_.files_merged;
  }

  if (!writer.Close()) {
    error_ = "flush " + part_path + ": " + writer.last_error();
    fs::remove(part_path, ec);
    return false;
  }

  fs::rename(part_path, final_path, ec);
  if (ec) {
    error_ = "rename " + part_path + " -> " + final_path + ": " + ec.message();
    return false;
  }
  stats_.outputs.push_back(final_path);
  ++stats_.windows_written;
  return true;
}

bool Merger::Run() {
  std::vector<InputFile> inputs;
  if (!ScanInputs(inputs)) return false;

  if (cfg_.kind == WindowKind::All) {
    // A skipped input in single-file mode is a missing span inside the one
    // output, not a per-window inconvenience that leaves the rest trustworthy.
    // Fail instead of producing a file with a hole in it.
    if (stats_.files_skipped > 0) {
      error_ = std::to_string(stats_.files_skipped) +
               " input file(s) were unusable; refusing a partial merge";
      return false;
    }
    if (inputs.empty()) {
      error_ = "no mergeable .mkn inputs in " + cfg_.input_dir;
      return false;
    }
    // Only the whole-directory mode can see a cross-file inversion; the
    // bucketed modes would just write it out one window at a time.
    if (cfg_.verify_timeline && !VerifyTimeline(inputs)) return false;
    return MergeWindow(inputs, /*window_key=*/0);
  }

  // Group by window key, on the SAME field the sort uses. Inputs are sorted by
  // last_ts_ns, so keying on last_ts_ns keeps each group contiguous; keying on
  // first_ts_ns here would let a group reappear after a later one had already
  // been written, because a stale early record can move a file's start into an
  // earlier window than the one its data ends in.
  size_t i = 0;
  while (i < inputs.size()) {
    const int64_t key = WindowKeyFromNs(inputs[i].last_ts_ns, cfg_.kind);
    size_t j = i;
    while (j < inputs.size() &&
           WindowKeyFromNs(inputs[j].last_ts_ns, cfg_.kind) == key) {
      ++j;
    }
    std::vector<InputFile> group(inputs.begin() + i, inputs.begin() + j);
    if (!MergeWindow(group, key)) return false;
    i = j;
  }
  return true;
}

}  // namespace mokuren::mdf
