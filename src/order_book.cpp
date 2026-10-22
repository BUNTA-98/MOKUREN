#include "order_book.hpp"

#include <cstring>

namespace mokuren {
namespace {

// qty <= 0 removes the level: that is how a delta expresses a level leaving
// the window, and erasing it here means the maps never hold a zero. a
// non-positive price is skipped rather than stored, which also rejects NaN.
template <typename Side>
void ApplyLevels(Side& side, const std::vector<OrderBook::Level>& levels) {
  for (const auto& [price, qty] : levels) {
    if (!(price > 0.0)) continue;
    if (qty <= 0.0) {
      side.erase(price);
    } else {
      side[price] = qty;
    }
  }
}

}  // namespace

OrderBook::ApplyResult OrderBook::ApplySnapshot(
    const std::vector<Level>& bids, const std::vector<Level>& asks,
    int64_t update_id) {
  bids_.clear();
  asks_.clear();
  ApplyLevels(bids_, bids);
  ApplyLevels(asks_, asks);
  // the snapshot is authoritative even if one side came back empty. the
  // sequence cursor must still advance, or the next delta looks like a gap and
  // provokes a resync for nothing.
  has_snapshot_ = true;
  if (update_id > 0) last_update_id_ = update_id;
  return ApplyResult::Applied;
}

OrderBook::ApplyResult OrderBook::ApplyDelta(
    const std::vector<Level>& bids, const std::vector<Level>& asks,
    int64_t update_id) {
  if (!has_snapshot_) return ApplyResult::Ignored;

  // a skipped update id leaves a removed level present or a grown level small,
  // and nothing downstream can tell: the frame that results is structurally
  // valid. resync rather than trade on it.
  if (update_id > 0 && last_update_id_ > 0 && update_id != last_update_id_ + 1) {
    return ApplyResult::GapDetected;
  }

  ApplyLevels(bids_, bids);
  ApplyLevels(asks_, asks);
  // advance the cursor only when the venue numbered this frame. assigning the
  // 0 fallback would silently disable gap detection for the rest of the
  // session, and a book that stopped being checked looks exactly like a book
  // that is correct.
  if (update_id > 0) last_update_id_ = update_id;
  return ApplyResult::Applied;
}

void OrderBook::Clear() {
  bids_.clear();
  asks_.clear();
  last_update_id_ = 0;
  has_snapshot_ = false;
}

L2Snapshot OrderBook::ToSnapshot(int64_t timestamp_ms) const {
  L2Snapshot snap{};
  std::memset(&snap, 0, sizeof(L2Snapshot));
  snap.timestamp = timestamp_ms;

  size_t n = 0;
  for (const auto& [price, qty] : bids_) {
    if (n >= static_cast<size_t>(MAX_L2_DEPTH)) break;
    snap.bids[n].price = price;
    snap.bids[n].qty = qty;
    ++n;
  }
  if (n > 0) {
    snap.best_bid_price = snap.bids[0].price;
    snap.best_bid_qty = snap.bids[0].qty;
  }

  n = 0;
  for (const auto& [price, qty] : asks_) {
    if (n >= static_cast<size_t>(MAX_L2_DEPTH)) break;
    snap.asks[n].price = price;
    snap.asks[n].qty = qty;
    ++n;
  }
  if (n > 0) {
    snap.best_ask_price = snap.asks[0].price;
    snap.best_ask_qty = snap.asks[0].qty;
  }

  return snap;
}

}  // namespace mokuren
