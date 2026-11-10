#pragma once

#include "l2_feed.hpp"
#include "trade_event.hpp"

#include <cstdint>
#include <vector>

// The engine graph is only dereferenced in run_loop.cpp, so this header stays
// free of factory.hpp / aggregator.hpp and the cost of including it is the
// book source plus a vector of trades.
struct EngineInstance;

namespace mokuren {

class PropGuard;
class SignalExecutor;

struct RunLoopStats {
  int signals_seen = 0;
  int signals_executed = 0;
  int signals_opened = 0;
  int signals_vetoed = 0;
  bool prop_breached = false;
  // Last timestamp of a book frame actually applied, or 0 when the run saw no
  // book at all. Reported so a caller can log the span; the forced close uses
  // it internally.
  int64_t session_end_ts = 0;
};

// The single implementation of "walk the prints, catch the book up, evaluate,
// execute". Every backtest path routes through this so the three things that
// had been duplicated across five drivers -- and were wrong in four of them --
// exist once:
//
//   1. THE BOOK TAIL DRAIN. Book frames after the last print are applied.
//      Without it a recording whose book outlives its last trade truncates at
//      that trade: book-only triggers never fire in the tail, and the prop
//      guard never sees the final mark.
//
//   2. THE END-PRICE FALLBACK. The final bar's close is 0.0 when that bar had
//      no prints (ResetBar zeroes it, ProcessTrade only restores it on the next
//      print, and FlushLastCandle skips an empty bar). Closing at 0.0 books a
//      phantom total loss, which is a whole permutation's net profit. Fall back
//      to the last traded price, then to the last close in history.
//
//   3. THE SESSION-END TIMESTAMP. The forced close is timestamped at the last
//      book frame applied, not at the final bar's OPEN -- which could be a full
//      interval early.
//
// `where` is the prop-guard label ("scan", "wfa_is", ...); it is the only thing
// a caller chooses that changes behaviour, and it only changes log text.
//
// Preconditions: `executor` holds pointers into `eng` and must not outlive it;
// both are expected to share the caller's scope. `prop_on` is the caller's
// reading of PropRules::enabled -- it is consulted here so a caller that passed
// false cannot have the guard silently refuse every entry.
RunLoopStats RunLoop(EngineInstance& eng,
                     SignalExecutor& executor,
                     PropGuard& prop_guard,
                     bool prop_on,
                     const char* where,
                     const std::vector<TradeEvent>& trades,
                     L2Feed& l2);

}  // namespace mokuren
