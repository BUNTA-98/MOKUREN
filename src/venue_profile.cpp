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
    // the venue pushes up to 20 levels per side; HyperliquidDataStreamer
    // truncates anything past MAX_L2_DEPTH.
    profile.l2_depth = 20;
    return profile;
  }

  // anything else: the public feed still has to be validated, so the strict
  // profile applies. an unknown name is not a pass -- it is the strictest
  // reading, which is the right default for a venue this build has no
  // contract with.
  return profile;
}

}  // namespace mokuren::integrity
