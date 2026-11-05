// Reads a dataset the way the backtest does and prints what came back.
//
// Deliberately goes through DataManager rather than market_data/reader.hpp.
// The raw reader can decode a file the engine still cannot use -- a factor of
// 1000 in the timestamps, an order the Slice* helpers rely on, a channel the
// dataset branch forgot to load -- and a check that bypasses the engine's entry
// point only proves the decoder agrees with itself. These are the same two
// functions the replay calls, on the same path.
//
// Usage:
//   mokuren_inspect recordings/btc_probe.mkn
//   mokuren_inspect data/historical/bybit_btc
//   mokuren_inspect merged/BTCUSDT-2026-10-29_2026-11-01.mkn --day 2026-10-30
//   mokuren_inspect merged/BTCUSDT-2026-10-29_2026-11-01.mkn --stats
//
// --day   decodes one UTC day of a .mkn recording. Needed whenever the file is
//         larger than RAM: the full decode materialises the whole book (688 B
//         per snapshot), so a multi-day merge cannot be read in one piece.
// --stats reads block headers only. No decode, no heap. It answers "is the
//         block chain walkable and how does it span" for a file too large to
//         decode at all -- and deliberately does NOT go through DataManager,
//         because reproducing the engine's view is exactly what does not fit.

#include "data_manager.hpp"
#include "logger.hpp"
#include "market_data/reader.hpp"  // IsSessionFile: the footprint preflight
#include "trade_event.hpp"

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace {

std::string Utc(int64_t ms) {
  if (ms <= 0) return "-";
  const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &seconds);
#else
  gmtime_r(&seconds, &tm);
#endif
  char buf[32] = {};
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

// SliceTrades and SliceL2 break out of their loop once they pass end_time,
// which is only correct on an ascending vector. An inverted pair makes every
// window in a WFA quietly short rather than visibly wrong, so it is worth
// counting: the CSV path happens to be ascending, the .mkn path sorts, and
// this is the line that tells them apart if either changes.
template <typename T>
size_t CountInversions(const std::vector<T>& records) {
  size_t inversions = 0;
  for (size_t i = 1; i < records.size(); ++i) {
    if (records[i].timestamp < records[i - 1].timestamp) ++inversions;
  }
  return inversions;
}

template <typename T>
void PrintSpan(const char* label, const std::vector<T>& records) {
  if (records.empty()) {
    std::cout << label << ": 0\n";
    return;
  }
  std::cout << label << ": " << records.size() << "\n"
            << "  first: " << Utc(records.front().timestamp) << " ("
            << records.front().timestamp << ")\n"
            << "  last:  " << Utc(records.back().timestamp) << " ("
            << records.back().timestamp << ")\n"
            << "  inversions: " << CountInversions(records) << "\n";
}

// Days since 1970-01-01 from a civil date. Hinnant's days_from_civil: exact for
// the whole int range and free of the timegm/_mkgmtime portability split that
// the rest of the codebase has to work around for local time.
int64_t DaysFromCivil(int y, int m, int d) {
  y -= (m <= 2) ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// "YYYY-MM-DD" -> the half-open UTC day as [start_ns, end_ns], end inclusive.
bool ParseUtcDay(const std::string& s, int64_t& start_ns, int64_t& end_ns) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
  for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  const int y = std::atoi(s.substr(0, 4).c_str());
  const int m = std::atoi(s.substr(5, 2).c_str());
  const int d = std::atoi(s.substr(8, 2).c_str());
  if (m < 1 || m > 12 || d < 1 || d > 31) return false;

  constexpr int64_t kNsPerDay = 86'400'000'000'000LL;
  const int64_t day = DaysFromCivil(y, m, d);
  start_ns = day * kNsPerDay;
  end_ns = start_ns + kNsPerDay - 1;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string dataset;
  std::string day;
  bool stats_only = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--stats") {
      stats_only = true;
    } else if (a == "--day") {
      if (i + 1 >= argc) {
        std::cerr << "--day needs a value (YYYY-MM-DD)\n";
        return EXIT_FAILURE;
      }
      day = argv[++i];
    } else if (a == "-h" || a == "--help") {
      std::cerr << "usage: mokuren_inspect <dataset> [--day YYYY-MM-DD] "
                   "[--stats]\n";
      return EXIT_SUCCESS;
    } else if (!a.empty() && a[0] == '-') {
      std::cerr << "unknown argument: " << a << "\n";
      return EXIT_FAILURE;
    } else if (dataset.empty()) {
      dataset = a;
    } else {
      std::cerr << "only one dataset per run\n";
      return EXIT_FAILURE;
    }
  }

  if (dataset.empty()) {
    std::cerr << "usage: mokuren_inspect <dataset> [--day YYYY-MM-DD] "
                 "[--stats]\n"
                 "\n"
                 "  <dataset> is a .mkn recording or a historical directory\n"
                 "  (trades.csv / L2.csv). prints what DataManager returns for\n"
                 "  it, which is exactly what the backtest consumes.\n"
                 "\n"
                 "  --day YYYY-MM-DD  decode one UTC day only (a .mkn recording\n"
                 "                    larger than RAM cannot be read whole)\n"
                 "  --stats           block headers only: span, record count,\n"
                 "                    worst intra-stream step back. no decode.\n";
    return EXIT_FAILURE;
  }

  // Console logging on: DataManager reports a truncated recording, a skipped
  // block or a checksum failure through the logger. A verification run that
  // swallows those reads a damaged file as a clean one.
  mokuren::InitLogger("logs/inspect.log", spdlog::level::info, true);

  std::cout << "dataset: " << dataset << "\n";

  int64_t start_ns = std::numeric_limits<int64_t>::min();
  int64_t end_ns   = std::numeric_limits<int64_t>::max();
  if (!day.empty()) {
    if (!ParseUtcDay(day, start_ns, end_ns)) {
      std::cerr << "bad --day '" << day << "': expected YYYY-MM-DD\n";
      mokuren::ShutdownLogger();
      return EXIT_FAILURE;
    }
    std::cout << "window: " << day << " 00:00:00-24:00:00 UTC\n";
  }

  // --stats: structure and counts, no decode. The only mode that works on a
  // file larger than RAM. It walks block headers and seeks over every payload,
  // so it costs seconds and no heap -- but it does NOT verify payload checksums.
  // A clean run means "the block chain is walkable", not "the records are
  // intact"; only a decode proves that.
  if (stats_only) {
    if (!mokuren::mdf::IsSessionFile(dataset)) {
      std::cerr << "--stats needs a .mkn recording\n";
      mokuren::ShutdownLogger();
      return EXIT_FAILURE;
    }
    mokuren::mdf::Reader reader;
    if (!reader.Open(dataset)) {
      std::cerr << "cannot open: " << reader.last_error() << "\n";
      mokuren::ShutdownLogger();
      return EXIT_FAILURE;
    }
    if (!reader.ScanChainBounds()) {
      std::cerr << "block chain unreadable: " << reader.last_error() << "\n";
      mokuren::ShutdownLogger();
      return EXIT_FAILURE;
    }
    const auto& m = reader.metadata();
    std::cout << "records: " << m.record_count << "\n"
              << "  first: " << Utc(m.first_ts_ns / 1'000'000) << "\n"
              << "  last:  " << Utc(m.last_ts_ns / 1'000'000) << "\n"
              // Recorded by the writer. Empty means the file predates the
              // field, which is how a mixed-version directory becomes visible
              // instead of having to be inferred from timestamps.
              << "  writer: "
              << (m.writer_version.empty() ? "(unrecorded)"
                                           : m.writer_version)
              << "\n";

    // Chain vs trailer index. Two measurements of the same file; they differ
    // exactly when the trailer is damaged, because index entries carry no
    // checksum -- only payloads do. The DELTA is the diagnosis: a constant
    // offset means an older writer stored payload_offset where block_offset
    // belongs (56 bytes, invisible to every bounds check), while a varying
    // offset means damage after the write.
    if (m.finalised) {
      const int64_t delta = static_cast<int64_t>(m.first_index_offset) -
                            static_cast<int64_t>(m.first_block_offset);
      if (reader.index_mismatch_count() > 0 || delta != 0) {
        std::cout << "  trailer index DISAGREES with the block chain:\n"
                  << "    entries that never matched a block: "
                  << reader.index_mismatch_count() << "\n"
                  << "    first entry offset " << m.first_index_offset
                  << ", first block offset " << m.first_block_offset
                  << " (delta " << delta << ")\n"
                  << "    a delta of exactly " << mokuren::mdf::kBlockHeaderSize
                  << " means payload_offset was stored instead of "
                     "block_offset\n"
                  << "    index entries parsed: " << reader.index_entries()
                  << ", blocks in the chain: " << reader.blocks_walked()
                  << "\n";
        // The writer's own entry size, recovered from the data. index_size is
        // fixed by the file; parsing it at kIndexEntrySize always yields
        // index_size/32 entries, so this ratio is what tells you how wide the
        // writer's IndexEntry actually was. 32 = this build's layout. Anything
        // else means that writer's table was read at the wrong stride, every
        // offset after the first is shifted, and no structural check could have
        // noticed -- index entries carry no checksum.
        if (reader.blocks_walked() > 0 && reader.index_entries() > 0) {
          const double implied =
              static_cast<double>(mokuren::mdf::kIndexEntrySize) *
              static_cast<double>(reader.index_entries()) /
              static_cast<double>(reader.blocks_walked());
          std::cout << "    implied entry size: " << implied << " bytes";
          if (implied < 31.5 || implied > 32.5) {
            std::cout << "  <-- NOT " << mokuren::mdf::kIndexEntrySize
                      << ": that writer used a different IndexEntry size";
          }
          std::cout << "\n";
        }
        std::cout << "    the chain is what is in the file; the counts and "
                     "span above are the chain's\n";
      }
    }
    if (m.max_inversion_ns > 0) {
      // Expected to be non-zero on a merge: the recorder keeps a late print,
      // and the merge reproduces it. A value near a whole day means a record
      // arrived with an old venue stamp -- which is why Run() needed
      // --no-verify for that directory.
      std::cout << "  worst intra-stream step back: "
                << (m.max_inversion_ns / 1'000'000) << " ms ("
                << Utc(m.max_inversion_prev_ns / 1'000'000) << " -> "
                << Utc(m.max_inversion_next_ns / 1'000'000) << ")\n";
    }
    mokuren::ShutdownLogger();
    return EXIT_SUCCESS;
  }

  // Footprint preflight, before anything expensive. DataManager materialises
  // both channels in RAM, and for a snapshot-only recording the decoded book
  // tracks the file size almost exactly: a book record is 680 bytes on disk
  // (BookSnapshotRec + kMaxBookDepth levels a side) and 688 bytes in
  // L2Snapshot, whose arrays are the same 2 x 20 x 16. A merged multi-day file
  // therefore needs roughly its own size in heap, plus 1.5x while the vector
  // reallocates.
  //
  // Printing it matters because the failure mode is not an error: on a machine
  // without that much RAM the decode does not stop, it starts swapping, and
  // since nothing is written until it returns, that is indistinguishable from
  // a hung process.
  if (mokuren::mdf::IsSessionFile(dataset)) {
    std::error_code ec;
    const uintmax_t bytes = std::filesystem::file_size(dataset, ec);
    if (!ec) {
      std::cout << "size: " << (bytes / (1024.0 * 1024.0 * 1024.0))
                << " GiB -- decoding needs about that much RAM for the book"
                << (day.empty() ? " (add --day to decode one UTC day)\n" : "\n");
    }
  }
  std::cout << std::flush;

  // Two passes on purpose: this is what the replay does (trades first, then
  // book), so a doubled read of a large recording is a property of the path
  // being measured, not an accident of this tool re-reading it.
  //
  // DataManager refuses a damaged recording instead of returning a short
  // channel; here that refusal IS the diagnosis, so print it and stop rather
  // than letting it escape as a stack trace.
  std::vector<TradeEvent> trades;
  std::vector<L2Snapshot> l2;
  // ONE decode for both channels. The two-call form (LoadAllTrades then
  // LoadAllL2Snapshots) was justified above as mirroring the replay, but
  // ReadSessionFile materialises BOTH channels on either call: the first call
  // built the whole 7 GB book only to throw it away, then the second built it
  // again. That is where the earlier run died, in the *trades* call. LoadAll is
  // the entry point for callers that need both, and the peak is the book either
  // way -- this only stops paying for it twice.
  std::cout << "loading..." << std::flush;
  try {
    DataManager::LoadedDataset loaded =
        day.empty() ? DataManager::LoadAll(dataset)
                    : DataManager::LoadAllRange(dataset, start_ns, end_ns);
    trades = std::move(loaded.trades);
    l2 = std::move(loaded.l2);
    std::cout << " done\n";
  } catch (const std::exception& e) {
    std::cout << "\n";
    std::cerr << "dataset refused: " << e.what() << "\n";
    mokuren::ShutdownLogger();
    return EXIT_FAILURE;
  }

  PrintSpan("trades", trades);
  PrintSpan("l2", l2);

  bool aligned = true;
  if (!trades.empty() && !l2.empty()) {
    aligned = DataManager::ValidateDataAlignment(
        trades, l2, [](const std::string& status) {
          std::cerr << "alignment: " << status << "\n";
        });
  }

  mokuren::ShutdownLogger();

  if (trades.empty() && l2.empty()) {
    std::cerr << "nothing loaded: the path holds no trades and no book\n";
    return EXIT_FAILURE;
  }
  if (!aligned) {
    std::cerr << "trades and book do not overlap in time: a replay would run "
                 "one channel blind\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
