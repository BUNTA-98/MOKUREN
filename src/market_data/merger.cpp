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

int64_t WindowKeyFromNs(int64_t ts_ns, WindowKind kind) {
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
    const auto& meta = reader.metadata();
    if (!meta.finalised) {
      ++stats_.files_skipped;
      continue;
    }
    if (meta.record_count == 0) {
      ++stats_.files_skipped;  // header-only or empty recording
      continue;
    }
    InputFile f;
    f.path        = path;
    f.first_ts_ns = meta.first_ts_ns;
    f.last_ts_ns  = meta.last_ts_ns;
    out.push_back(std::move(f));
  }

  // Order by index first_ts_ns, never by filename.
  std::sort(out.begin(), out.end(),
            [](const InputFile& a, const InputFile& b) {
              return a.first_ts_ns < b.first_ts_ns;
            });
  return true;
}

bool Merger::MergeWindow(const std::vector<InputFile>& files,
                         int64_t window_key) {
  const std::string label = WindowLabelFromKey(window_key, cfg_.kind);
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
    stats_.records_copied += reader.record_count();
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
  ++stats_.windows_written;
  return true;
}

bool Merger::Run() {
  std::vector<InputFile> inputs;
  if (!ScanInputs(inputs)) return false;

  // Group by window key. Inputs are already sorted by first_ts_ns, so each
  // group is contiguous and its files are in time order.
  size_t i = 0;
  while (i < inputs.size()) {
    const int64_t key = WindowKeyFromNs(inputs[i].first_ts_ns, cfg_.kind);
    size_t j = i;
    while (j < inputs.size() &&
           WindowKeyFromNs(inputs[j].first_ts_ns, cfg_.kind) == key) {
      ++j;
    }
    std::vector<InputFile> group(inputs.begin() + i, inputs.begin() + j);
    if (!MergeWindow(group, key)) return false;
    i = j;
  }
  return true;
}

}  // namespace mokuren::mdf
