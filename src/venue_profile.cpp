#include "venue_profile.hpp"

#include "trade_event.hpp"

#include <algorithm>

namespace mokuren::integrity {

VenueProfile ProfileForVenue(const std::string& venue_name) {
  VenueProfile profile;
  profile.venue = venue_name;

  if (venue_name == "bybit") {
    // v5 public streams: publicTrade + orderbook.50, sequenced.
    profile.has_sequence_numbers = true;
    // BybitDataStreamer now subscribes to orderbook.50 and maintains the book
    // from the snapshot+delta stream. declared post-truncation on purpose: the
    // gate uses this number as a capability check against min_l2_depth, so it
    // has to be what the engine can actually receive, not what the socket
    // carries. claiming 50 while the streamer truncates at MAX_L2_DEPTH would
    // let a min_l2_depth of 30 pass the pre-check and then reject every frame
    // as shallow -- the exact failure the pre-check exists to prevent.
    profile.l2_depth = std::min<size_t>(50, static_cast<size_t>(MAX_L2_DEPTH));
    return profile;
  }

  if (venue_name == "hyperliquid") {
    // l2Book is a whole-book snapshot on every push and carries no seq.
    profile.has_sequence_numbers = false;
    // measured, not assumed: four recordings between 30 s and 5 min all produced
    // 11-12 book frames per minute (~5.3 s between snapshots) against ~330
    // trades per minute, with zero frames dropped at socket, envelope, parse or
    // write. so each book frame is the context for ~30 prints, and replay sees a
    // book up to 5 s old. that is the venue's cadence, not a gap in the
    // recording -- a Hyperliquid L2 backtest is therefore not comparable to a
    // bybit one on the same strategy, and its depth readings should not be read
    // as if it were.
    // the venue pushes up to 20 levels per side, and HyperliquidDataStreamer
    // truncates anything past MAX_L2_DEPTH. clamped for the same reason as
    // bybit: this number is what the startup gate checks min_l2_depth against,
    // so it has to be what the engine can actually receive. declaring 20 while
    // the streamer truncates lower would let a min_l2_depth in between pass the
    // pre-check and then reject every live frame as shallow.
    profile.l2_depth = std::min<size_t>(20, static_cast<size_t>(MAX_L2_DEPTH));
    // measured floor, not the average: four recordings between 30s and 5min
    // produced 11-12 frames/min each, including the 5-minute one. declared as
    // 10 so a healthy session clears it with a frame to spare, while a feed
    // degraded to a trickle still fails -- 3/min is not a quiet venue, it is a
    // broken one. without this the gate applies a delta feed's 120-300/min and
    // refuses to arm on the venue above.
    profile.l2_frames_per_min = 10;
    return profile;
  }

  // anything else: the public feed still has to be validated, so the strict
  // profile applies. an unknown name is not a pass -- it is the strictest
  // reading, which is the right default for a venue this build has no
  // contract with.
  return profile;
}

}  // namespace mokuren::integrity
