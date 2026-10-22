#pragma once

#include "trade_event.hpp"

#include <cstdint>

namespace mokuren::integrity {

// Severity is ordered: Ok < Recoverable < Fatal. multi-check passes keep the
// worst verdict, so a book with a zero-qty level and a NaN price reports the
// NaN and not whichever check happened to run last.
enum class Severity : uint8_t { Ok = 0, Recoverable = 1, Fatal = 2 };

enum class Code : uint8_t {
  Ok = 0,

  // --- trade print ---------------------------------------------------------
  TradePriceNotFinite,
  TradeQuantityNotFinite,
  TradePriceNotPositive,
  TradeQuantityNegative,
  TradeQuantityZero,
  TradeTimestampNotPositive,
  TradeTimestampBackwards,

  // --- l2 snapshot ---------------------------------------------------------
  BookTimestampNotPositive,
  BookTimestampBackwards,
  BookEmpty,
  BookSideEmpty,
  BookTooShallow,
  BookLevelPriceNotFinite,
  BookLevelQuantityNotFinite,
  BookQuantityNegative,
  BookQuantityZero,
  BookBidsNotDescending,
  BookAsksNotAscending,
  BookLocked,
  BookCrossed,

  // --- transport -----------------------------------------------------------
  TransportError,
  StreamEndedEarly,
};

// human-readable name of a violation. only called when a report is formatted,
// never on the streamer's callback thread.
const char* CodeName(Code code);

struct Verdict {
  Severity severity = Severity::Ok;
  Code code = Code::Ok;

  bool ok() const { return severity == Severity::Ok; }
  bool fatal() const { return severity == Severity::Fatal; }
};

struct BookLimits {
  // a single backwards timestamp is a reordering artefact; a venue that
  // declares monotonic sequencing must not have any.
  bool require_monotonic_timestamps = false;

  // levels per side a frame must carry to be usable. 0 disables the check,
  // which is what the ungated path (ProbeCriteria::Disabled) uses.
  //
  // this has to be an explicit requirement rather than "whatever arrived":
  // one populated level per side is a structurally valid L2Snapshot, so a
  // depth-1 feed passes every other rule here while StackedImbalance and
  // SpoofHunter index bids[1]/asks[1] and read zeroed slots. that is not a
  // weaker signal, it is a confidently wrong one.
  size_t min_depth = 0;
};

// Pure checks. The caller owns the cursor, so the same rules run on the
// startup probe, on the live stream and on the replay decoder, and can be
// unit-tested on a struct literal without a socket or a clock.
//
// rejected frames are rejected here and never reach the engine: a malformed
// TradeEvent writing NaN into the aggregator is exactly what this exists to
// stop, so the check must happen before the forward, not after.
Verdict ValidateTrade(const TradeEvent& t, int64_t& last_ts);
Verdict ValidateL2(const L2Snapshot& s, const BookLimits& limits,
                   int64_t& last_ts);

// The sentinel both streamers emit on reconnect, unexpected close and
// transport error: every field zeroed except the receive timestamp. It is not
// a tradeable book and must never reach the engine as one -- it is the cue to
// stop trusting the local book until a real snapshot arrives.
//
// The shape is defined HERE and nowhere else: the producers (streamers) and
// the consumer (this validator) must not each keep their own copy of what a
// gap looks like, or the day one of them grows a field the other stops
// recognising the drop.
bool IsBookGap(const L2Snapshot& s);
L2Snapshot MakeBookGap(int64_t timestamp_ms);

}  // namespace mokuren::integrity
