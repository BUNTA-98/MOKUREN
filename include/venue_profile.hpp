#pragma once

#include <cstddef>
#include <string>

namespace mokuren::integrity {

// What one venue promises about its feed.
//
// This is the ONLY place a venue's quirks are allowed to influence a verdict:
// the checks themselves are shared, so a new venue cannot "pass" by growing a
// weaker validator. Adding an exchange means adding a streamer and one case
// here -- no engine edits.
//
// The defaults are the strictest possible reading, and they are what an
// unknown venue name gets: every channel required, nothing tolerated.
struct VenueProfile {
  std::string venue = "unknown";

  // Channels the gate must see well-formed traffic on before the engine is
  // armed. A channel that stays silent fails the probe; it does not "pass by
  // silence". Both default to true because a half-blind engine is worse than
  // no engine: trades alone cannot drive the depth triggers, and l2 alone
  // closes no candle.
  bool trades_required = true;
  bool l2_required = true;

  // Bybit sends snapshot+delta and can be reconciled from seq; Hyperliquid
  // pushes a full l2Book frame every time. Declared, never assumed, so a venue
  // that cannot be reconciled is not measured against a rule it cannot meet.
  bool has_sequence_numbers = false;

  // levels per side this venue's public book feed actually carries. 0 means
  // "not declared": the frame-level depth check still runs, but there is no
  // early cross-check against the strategy's requirement.
  //
  // a FACT about the feed, not a requirement. it exists so a venue whose book
  // can never satisfy the configured triggers is rejected at startup with a
  // reason, instead of passing the probe on a technically valid depth-1 frame.
  size_t l2_depth = 0;
};

// One case per integrated venue. Unknown names get the strict defaults.
VenueProfile ProfileForVenue(const std::string& venue_name);

}  // namespace mokuren::integrity
