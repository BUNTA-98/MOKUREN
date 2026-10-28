#include "recorder.hpp"

#include "data_integrity.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <utility>

namespace mokuren {

int64_t DayKeyFromNs(int64_t ts_ns) {
  // Truncation toward zero: fine, market data is always post-1970.
  return ts_ns / kNsPerDay;
}

std::string DayStringFromKey(int64_t day_key) {
  // non-const: gmtime_s on Windows takes time_t*.
  std::time_t secs = static_cast<std::time_t>(day_key * 86400);
  std::tm tm_utc{};
#if defined(_WIN32)
  gmtime_s(&tm_utc, &secs);
#else
  gmtime_r(&secs, &tm_utc);
#endif
  char buf[16];
  std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm_utc);
  return buf;
}

namespace {

// The engine runs in milliseconds (timestamp_utils.hpp normalises everything to
// ms on ingest) and the .mkn format stores nanoseconds. One integer shift, at
// the only boundary where the two units meet.
int64_t MsToNs(int64_t ms) { return ms * 1'000'000; }

// When the frame reached the recorder. TradeEvent and L2Snapshot carry only the
// exchange stamp, so this cannot be the socket's arrival time -- but the
// callback runs immediately after the streamer emits, which makes it the
// closest honest measurement and the only field that can show a late frame or a
// venue clock offset. Copying the exchange stamp instead made both invisible: a
// session whose first trade is timestamped 8s before its own filename reads as
// perfectly consistent.
int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// "HHMMSS" UTC, used only as a disambiguating suffix on restart collisions.
std::string TimeSuffix(int64_t ts_ns) {
  const int64_t sod = (ts_ns / 1'000'000'000LL) % 86400;
  const int64_t s = sod < 0 ? sod + 86400 : sod;
  char buf[8];
  std::snprintf(buf, sizeof buf, "%02lld%02lld%02lld",
                static_cast<long long>(s / 3600),
                static_cast<long long>((s / 60) % 60),
                static_cast<long long>(s % 60));
  return buf;
}

bool PathExists(const std::string& p) {
  std::error_code ec;
  return std::filesystem::exists(p, ec);
}

}  // namespace

MarketDataRecorder::MarketDataRecorder(RecorderConfig cfg)
    : cfg_(std::move(cfg)) {
  // Validated frames are the only ones written, so the depth requirement has to
  // be a real gate rather than a comment: 0 would disable it.
  if (cfg_.min_book_depth == 0) cfg_.min_book_depth = 1;
}

MarketDataRecorder::~MarketDataRecorder() {
  // Idempotent; finalises (flushes + renames) a daily file still open.
  if (opened_ || writer_.is_open()) Close();
}

void MarketDataRecorder::Attach(IMarketStreamer& streamer) {
  streamer.SetTradeCallback([this](const TradeEvent& t) { OnTrade(t); });
  streamer.SetL2Callback([this](const L2Snapshot& s) { OnL2(s); });
}

bool MarketDataRecorder::Open() {
  mdf::SessionInfo info;
  info.venue = cfg_.venue;
  info.env = cfg_.env;
  info.category = cfg_.category;
  info.symbol = cfg_.symbol;
  info.source = "live";

  if (!cfg_.daily) {
    opened_ = writer_.Open(cfg_.path, info);
    return opened_;
  }

  // Daily: create the instrument directory now, the file lazily -- a stream
  // that never ticks must not leave an empty header behind.
  const std::string sub = cfg_.venue + "-" + cfg_.symbol;
  dir_ = (std::filesystem::path(cfg_.path) / sub).string();
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  if (ec) {
    error_ = "create_directories " + dir_ + ": " + ec.message();
    return false;
  }
  current_day_ = -1;
  current_path_.clear();
  current_part_path_.clear();
  error_.clear();
  opened_ = true;
  return true;
}

bool MarketDataRecorder::Close() {
  opened_ = false;
  if (!cfg_.daily) return writer_.is_open() ? writer_.Close() : true;
  return CloseCurrentFile();
}

bool MarketDataRecorder::EnsureOpenFor(int64_t ts_ns) {
  if (!cfg_.daily) return writer_.is_open();
  if (current_day_ < 0) return OpenDayFor(ts_ns);
  return RotateIfNeeded(ts_ns);
}

bool MarketDataRecorder::RotateIfNeeded(int64_t ts_ns) {
  const int64_t day = DayKeyFromNs(ts_ns);

  // Same day, or a late/out-of-order record: keep the current file. A late
  // print is preserved rather than dropped -- a minor ordering imperfection
  // inside a day is a lesser evil than losing a record.
  if (day <= current_day_) return true;

  if (!CloseCurrentFile()) return false;  // not flushed; do not rotate
  if (!OpenDayFor(ts_ns)) return false;   // current_day_ stale -> retried

  ++stats_.rotations;
  return true;
}

bool MarketDataRecorder::OpenDayFor(int64_t ts_ns) {
  const int64_t     day  = DayKeyFromNs(ts_ns);
  const std::string date = DayStringFromKey(day);
  const std::string base = cfg_.venue + "-" + cfg_.symbol + "-" + date;

  std::string final_path = dir_ + "/" + base + ".mkn";

  if (cfg_.overwrite_existing) {
    std::error_code ec;
    std::filesystem::remove(final_path, ec);
    std::filesystem::remove(final_path + ".part", ec);
  } else if (PathExists(final_path) || PathExists(final_path + ".part")) {
    const std::string suffix = TimeSuffix(ts_ns);
    final_path = dir_ + "/" + base + "T" + suffix + ".mkn";
    for (int n = 1;
         PathExists(final_path) || PathExists(final_path + ".part"); ++n) {
      final_path = dir_ + "/" + base + "T" + suffix + "-" +
                   std::to_string(n) + ".mkn";
    }
  }

  const std::string part_path = final_path + ".part";

  mdf::SessionInfo info;
  info.venue = cfg_.venue;
  info.env = cfg_.env;
  info.category = cfg_.category;
  info.symbol = cfg_.symbol;
  info.source = "live";

  if (!writer_.Open(part_path, info)) {
    error_ = "open " + part_path + ": " + writer_.last_error();
    return false;
  }
  current_path_      = final_path;
  current_part_path_ = part_path;
  current_day_       = day;
  error_.clear();
  return true;
}

bool MarketDataRecorder::CloseCurrentFile() {
  if (!writer_.is_open()) {
    current_path_.clear();
    current_part_path_.clear();
    return true;
  }
  if (!writer_.Close()) {
    error_ = "flush " + current_part_path_ + ": " + writer_.last_error();
    current_path_.clear();
    current_part_path_.clear();
    return false;
  }

  std::error_code ec;
  std::filesystem::rename(current_part_path_, current_path_, ec);
  if (ec) {
    // Data is safe -- Close() wrote the trailer -- just under the .part name.
    // Report it and carry on; the merger skips *.part by design.
    error_ = "rename " + current_part_path_ + " -> " + current_path_ +
             ": " + ec.message();
  }
  current_path_.clear();
  current_part_path_.clear();
  return true;
}

void MarketDataRecorder::OnTrade(const TradeEvent& t) {
  ++stats_.trades_seen;
  if (!opened_) return;

  if (cfg_.validate) {
    const integrity::Verdict v = integrity::ValidateTrade(t, last_trade_ts_);
    if (!v.ok()) {
      ++stats_.trades_rejected;
      return;
    }
  }

  mdf::TradeRec rec{};
  rec.ts_exchange_ns = MsToNs(t.timestamp);
  rec.ts_recv_ns = NowNs();  // see NowNs: not a copy of the exchange clock
  rec.seq = 0;  // the public trade stream has no id we persist
  rec.price = t.price;
  rec.qty = t.quantity;
  // The format stores the AGGRESSOR, never the venue's "is buyer maker"
  // boolean: that convention flips sign between venues and would invert
  // footprint and delta on import.
  rec.aggressor = t.is_buyer_maker
                      ? static_cast<uint8_t>(mdf::Aggressor::Sell)
                      : static_cast<uint8_t>(mdf::Aggressor::Buy);

  if (!EnsureOpenFor(rec.ts_exchange_ns)) {
    ++stats_.trades_rejected;
    return;
  }

  if (writer_.WriteTrade(rec)) {
    ++stats_.trades_written;
  } else {
    ++stats_.trades_rejected;
  }
}

void MarketDataRecorder::OnL2(const L2Snapshot& s) {
  ++stats_.l2_seen;
  if (!opened_) return;

  // The reconnect sentinel is not a book and has no levels to record. Counting
  // it separately keeps "frames rejected as malformed" honest: a gap marker is
  // well-formed, it just means the local book is unknown.
  if (integrity::IsBookGap(s)) {
    ++stats_.l2_gaps;
    return;
  }

  if (cfg_.validate) {
    integrity::BookLimits limits;
    limits.require_monotonic_timestamps = cfg_.require_monotonic_timestamps;
    limits.min_depth = cfg_.min_book_depth;
    const integrity::Verdict v =
        integrity::ValidateL2(s, limits, last_book_ts_);
    if (!v.ok()) {
      ++stats_.l2_rejected;
      return;
    }
  }

  if (cfg_.l2_min_interval_ms > 0 && last_written_book_ts_ > 0 &&
      s.timestamp - last_written_book_ts_ < cfg_.l2_min_interval_ms) {
    ++stats_.l2_throttled;
    return;
  }

  // The streamer already reconstructed the book, so each L2Snapshot is a full
  // frame: bids then asks, best-first, unused slots zeroed. The legacy best_*
  // fields are derived from the arrays by the engine, so they are not stored
  // separately -- one source of truth in the file.
  mdf::BookLevel levels[static_cast<size_t>(MAX_L2_DEPTH) * 2];
  uint32_t n_bids = 0;
  uint32_t n_asks = 0;

  for (int i = 0; i < MAX_L2_DEPTH; ++i) {
    if (!(s.bids[i].price > 0.0)) break;
    levels[n_bids] = mdf::BookLevel{s.bids[i].price, s.bids[i].qty};
    ++n_bids;
  }
  for (int i = 0; i < MAX_L2_DEPTH; ++i) {
    if (!(s.asks[i].price > 0.0)) break;
    levels[n_bids + n_asks] = mdf::BookLevel{s.asks[i].price, s.asks[i].qty};
    ++n_asks;
  }

  mdf::BookSnapshotRec rec{};
  rec.ts_exchange_ns = MsToNs(s.timestamp);
  rec.ts_recv_ns = NowNs();  // see OnTrade
  rec.seq = 0;
  rec.n_bids = n_bids;
  rec.n_asks = n_asks;
  rec.flags = 0;

  if (!EnsureOpenFor(rec.ts_exchange_ns)) {
    ++stats_.l2_rejected;
    return;
  }

  if (writer_.WriteSnapshot(rec, levels, n_bids + n_asks)) {
    ++stats_.l2_written;
    last_written_book_ts_ = s.timestamp;
  } else {
    ++stats_.l2_rejected;
  }
}

}  // namespace mokuren
