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

#include "data_manager.hpp"
#include "logger.hpp"
#include "trade_event.hpp"

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <iostream>
#include <string>
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

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: mokuren_inspect <dataset>\n"
                 "\n"
                 "  <dataset> is a .mkn recording or a historical directory\n"
                 "  (trades.csv / L2.csv). prints what DataManager returns for\n"
                 "  it, which is exactly what the backtest consumes.\n";
    return EXIT_FAILURE;
  }

  const std::string dataset = argv[1];

  // Console logging on: DataManager reports a truncated recording, a skipped
  // block or a checksum failure through the logger. A verification run that
  // swallows those reads a damaged file as a clean one.
  mokuren::InitLogger("logs/inspect.log", spdlog::level::info, true);

  std::cout << "dataset: " << dataset << "\n";

  // Two passes on purpose: this is what the replay does (trades first, then
  // book), so a doubled read of a large recording is a property of the path
  // being measured, not an accident of this tool re-reading it.
  //
  // DataManager refuses a damaged recording instead of returning a short
  // channel; here that refusal IS the diagnosis, so print it and stop rather
  // than letting it escape as a stack trace.
  std::vector<TradeEvent> trades;
  std::vector<L2Snapshot> l2;
  try {
    trades = DataManager::LoadAllTrades(dataset);
    l2 = DataManager::LoadAllL2Snapshots(dataset);
  } catch (const std::exception& e) {
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
