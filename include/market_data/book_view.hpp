#pragma once

// Engine-facing view of an order book. This is *not* the on-disk layout and
// is free to change per release. It is a borrowed pointer + counts over
// whatever storage the reader (replay) or the book maintainer (live) owns,
// so passing it into a MarketContext never copies the levels.
//
// L1 is not a special case: n_bids == n_asks == 1.
// Depth is per side because thin books are asymmetric.

#include "market_data/format.hpp"

#include <cstdint>
#include <type_traits>

namespace mokuren::mdf {

inline constexpr int64_t kNoSeq = 0;

struct BookView {
  const BookLevel* bids   = nullptr;
  const BookLevel* asks   = nullptr;
  uint32_t         n_bids = 0;
  uint32_t         n_asks = 0;
  int64_t          ts_exchange_ns = 0;
  int64_t          ts_recv_ns     = 0;
  int64_t          seq            = kNoSeq;
  uint8_t          flags          = 0;

  bool valid() const        { return bids != nullptr && asks != nullptr; }
  bool empty() const        { return n_bids == 0 && n_asks == 0; }
  bool is_resync() const    { return (flags & snapshot_flags::kResync)    != 0; }
  bool is_truncated() const { return (flags & snapshot_flags::kTruncated) != 0; }
  bool is_stale() const     { return (flags & snapshot_flags::kStale)     != 0; }

  double best_bid() const { return n_bids ? bids[0].price : 0.0; }
  double best_ask() const { return n_asks ? asks[0].price : 0.0; }

  // Total resting quantity across the top-k levels of one side.
  double depth_qty(uint32_t k, bool bid_side) const {
    const BookLevel* lv = bid_side ? bids : asks;
    const uint32_t   n  = bid_side ? n_bids : n_asks;
    const uint32_t   m  = k < n ? k : n;
    double total = 0.0;
    for (uint32_t i = 0; i < m; ++i) total += lv[i].qty;
    return total;
  }
};
static_assert(std::is_trivially_copyable<BookView>::value,
              "BookView must be trivially copyable");

// What the dataset can give us. Populated from metadata at open time.
struct DataCapabilities {
  Granularity granularity          = Granularity::Tob;
  uint32_t    max_depth_bid        = 0;
  uint32_t    max_depth_ask        = 0;
  bool        has_trades           = false;
  bool        has_book             = false;
  bool        has_seq              = false;
  bool        has_recv_time        = false;
  int64_t     min_event_spacing_ns = 0;
  int64_t     max_event_spacing_ns = 0;
};

// What a trigger actually needs. Checked against DataCapabilities at startup
// so that a depth-starved trigger aborts with a readable error instead of
// silently returning SignalDirection::NONE forever.
struct TriggerRequirements {
  uint32_t min_depth_bid  = 0;
  uint32_t min_depth_ask  = 0;
  bool     require_trades = false;
  bool     require_seq    = false;
};

inline bool MeetsRequirements(const DataCapabilities& caps,
                              const TriggerRequirements& req) {
  if (caps.max_depth_bid < req.min_depth_bid) return false;
  if (caps.max_depth_ask < req.min_depth_ask) return false;
  if (req.require_trades && !caps.has_trades) return false;
  if (req.require_seq    && !caps.has_seq)    return false;
  return true;
}

} // namespace mokuren::mdf
