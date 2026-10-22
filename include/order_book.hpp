#pragma once

#include "trade_event.hpp"

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace mokuren {

// Local reconstruction of one venue's book from a snapshot+delta feed.
//
// Deliberately separate from the streamer that drives it. This is the only
// stateful logic between the socket and a validated L2Snapshot, and while it
// lived inside BybitDataStreamer the only way to exercise it was a live
// socket. A book that is quietly wrong still produces frames that pass every
// validator rule -- depth and ordering are self-consistency checks, not
// correctness -- so "it looked right in production" is not coverage.
class OrderBook {
 public:
  // one level as it arrives on the wire.
  using Level = std::pair<double, double>;  // (price, qty)

  enum class ApplyResult : uint8_t {
    Applied = 0,      // frame changed the book
    Ignored = 1,      // unusable: delta with no snapshot behind it, or empty
    GapDetected = 2,  // update id skipped: the book is no longer trustworthy
  };

  // Replace the book outright. `update_id` is the venue's sequence number, 0
  // when the venue does not number its frames (0 is never a valid id).
  ApplyResult ApplySnapshot(const std::vector<Level>& bids,
                            const std::vector<Level>& asks, int64_t update_id);

  // Apply changed levels. `qty <= 0` removes the level, which is how the wire
  // expresses a level leaving the window. Ignored (not applied) when no
  // snapshot has been seen: a delta applied to nothing makes a book that is
  // half real and half invented.
  ApplyResult ApplyDelta(const std::vector<Level>& bids,
                         const std::vector<Level>& asks, int64_t update_id);

  // Drop everything, including the sequence cursor. Called on every socket
  // open, close and error: the accumulated book belongs to a connection (or a
  // point in the sequence) that no longer holds.
  void Clear();

  // true when either side is empty: a resync in progress is not a market
  // state, and the caller must not publish it.
  bool empty() const { return bids_.empty() || asks_.empty(); }
  bool has_snapshot() const { return has_snapshot_; }
  size_t bid_depth() const { return bids_.size(); }
  size_t ask_depth() const { return asks_.size(); }
  int64_t last_update_id() const { return last_update_id_; }

  // Top of the book, truncated to MAX_L2_DEPTH levels per side and laid out the
  // way the validator reads it: bids descending, asks ascending, unused slots
  // zeroed, best_* taken from the arrays. Never call this on an empty side and
  // publish the result -- that frame is fatal to the engine.
  L2Snapshot ToSnapshot(int64_t timestamp_ms) const;

 private:
  std::map<double, double, std::greater<double>> bids_;
  std::map<double, double> asks_;
  int64_t last_update_id_ = 0;
  bool has_snapshot_ = false;
};

}  // namespace mokuren
