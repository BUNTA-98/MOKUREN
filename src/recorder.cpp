#include "recorder.hpp"

#include "data_integrity.hpp"

#include <utility>

namespace mokuren {
namespace {

// The engine runs in milliseconds (timestamp_utils.hpp normalises everything to
// ms on ingest) and the .mkn format stores nanoseconds. One integer shift, at
// the only boundary where the two units meet.
int64_t MsToNs(int64_t ms) { return ms * 1'000'000; }

}  // namespace

MarketDataRecorder::MarketDataRecorder(RecorderConfig cfg)
    : cfg_(std::move(cfg)) {
  // Validated frames are the only ones written, so the depth requirement has to
  // be a real gate rather than a comment: 0 would disable it.
  if (cfg_.min_book_depth == 0) cfg_.min_book_depth = 1;
}

MarketDataRecorder::~MarketDataRecorder() {
  // Idempotent. A recorder destroyed without Close() still leaves a readable
  // file as long as the producer has stopped; if it has not, that is the caller
  // bug the Stop()-before-Close() rule exists to prevent.
  if (writer_.is_open()) writer_.Close();
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
  return writer_.Open(cfg_.path, info);
}

bool MarketDataRecorder::Close() { return writer_.Close(); }

void MarketDataRecorder::OnTrade(const TradeEvent& t) {
  ++stats_.trades_seen;
  if (!writer_.is_open()) return;

  if (cfg_.validate) {
    const integrity::Verdict v = integrity::ValidateTrade(t, last_trade_ts_);
    if (!v.ok()) {
      ++stats_.trades_rejected;
      return;
    }
  }

  mdf::TradeRec rec{};
  rec.ts_exchange_ns = MsToNs(t.timestamp);
  // TradeEvent carries no local receive stamp. Copying the exchange time keeps
  // the field inside the record's contract (non-zero, monotonic per block)
  // instead of writing the 0 sentinel, which reads as "unknown" and would make
  // a latency analysis silently divide by a missing value.
  rec.ts_recv_ns = rec.ts_exchange_ns;
  rec.seq = 0;  // the public trade stream has no id we persist
  rec.price = t.price;
  rec.qty = t.quantity;
  // The format stores the AGGRESSOR, never the venue's "is buyer maker"
  // boolean: that convention flips sign between venues and would invert
  // footprint and delta on import.
  rec.aggressor = t.is_buyer_maker
                      ? static_cast<uint8_t>(mdf::Aggressor::Sell)
                      : static_cast<uint8_t>(mdf::Aggressor::Buy);

  if (writer_.WriteTrade(rec)) {
    ++stats_.trades_written;
  }
}

void MarketDataRecorder::OnL2(const L2Snapshot& s) {
  ++stats_.l2_seen;
  if (!writer_.is_open()) return;

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
  rec.ts_recv_ns = rec.ts_exchange_ns;  // see OnTrade
  rec.seq = 0;
  rec.n_bids = n_bids;
  rec.n_asks = n_asks;
  rec.flags = 0;

  if (writer_.WriteSnapshot(rec, levels, n_bids + n_asks)) {
    ++stats_.l2_written;
    last_written_book_ts_ = s.timestamp;
  }
}

}  // namespace mokuren
