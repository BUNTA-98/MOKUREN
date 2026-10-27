#pragma once

#include <cstddef>
#include <cstdint>
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

  // book frames per minute this venue's public l2 feed actually sustains.
  // 0 means "not declared", which leaves the configured rate requirement
  // exactly as the operator wrote it -- the strict reading an unknown venue
  // gets.
  //
  // a FACT about the feed, like l2_depth, and it exists for the same reason.
  // min_l2_frames_per_min describes a healthy DELTA feed: "the book changed at
  // least this often". a venue that republishes the whole book every ~5s
  // instead of a delta per change can never clear a delta feed's number, so the
  // gate would refuse a faithful feed while a silent one still failed -- the
  // requirement stops measuring health and starts measuring the wrong
  // architecture. declaring the cadence lets the gate ask for what the venue
  // can deliver without lowering the bar for a venue that can deliver more.
  uint64_t l2_frames_per_min = 0;
};

// One case per integrated venue. Unknown names get the strict defaults.
VenueProfile ProfileForVenue(const std::string& venue_name);

}  // namespace mokuren::integrity
