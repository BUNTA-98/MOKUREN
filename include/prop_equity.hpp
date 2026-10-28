#pragma once

#include "ibroker.hpp"
#include "logger.hpp"
#include "prop_guard.hpp"

#include <cmath>
#include <string>

namespace mokuren {

// Shared per-run prop-guard plumbing for the backtest, WFA and replay paths.
//
// The live orchestrator asks the active broker for its mark-to-market equity
// and feeds it to PropGuard::Observe on every print. The backtest paths have
// no venue, so they ask PaperTrader the same question at the price of the print
// being evaluated. This header is the one place that derivation lives, so
// scan, WFA-IS, WFA-OOS and replay cannot drift.
//
// NOTE: it does NOT assemble equity itself any more. it used to compute
// GetBalance() plus a gross floating leg, which ignored the exit fee and
// slippage PaperTrader::GetEquity() deducts -- so the backtest guard measured a
// slightly better account than --paper and --live, on the same limits. the
// floating leg belongs to whoever holds the position, and only the broker
// knows the costs it would pay to leave it.
//
// A fresh PropGuard starts Unarmed and refuses entries until the first
// Observe() with a positive equity reading. Callers must therefore call
// ObserveProp() before the first signal gate -- not just periodically -- or
// the first trade's signals are silently dropped.
inline double PaperEquity(IBroker* broker, double mark) {
  // one definition of equity, in the broker that owns the position. the mark is
  // explicit so this reads the same price the caller is evaluating rather than
  // whichever print last ticked the account.
  if (!broker) return 0.0;
  return broker->GetEquityAt(mark);
}

// mark-to-market equity with a direction-aware executable close: bid for a
// long, ask for a short. callers that hold both sides of the book must use
// this over GetEquityAt(mark), which treats its argument as a mid and slips
// it direction-aware -- correct for a trade print, wrong for a one-sided
// quote (best_bid for a short exits at best_bid * (1 + slip), i.e. on the
// wrong side of the book).
inline double PaperEquityBidAsk(IBroker* broker, double bid, double ask) {
  if (!broker) return 0.0;
  return broker->GetEquityAtBidAsk(bid, ask);
}

// Observe equity and, on the transition into a breach, flatten the open
// position once. Returns the decision so the caller can gate new entries on
// allow_new_entry(). The flatten uses ClosePartial(1.0, ..., "prop_guard")
// -- the same call live makes -- so the exit reason lands in
// TradeRecord::exit_reason and the UI inspector shows it like live does.
inline PropDecision ObserveProp(PropGuard& guard, IBroker* broker,
                                double mark, int64_t ts,
                                const std::string& tag) {
  const double equity = PaperEquity(broker, mark);
  const PropDecision d = guard.Observe(equity, ts);

  if (d.should_flatten && mark > 0.0 && broker && broker->HasOpenPosition()) {
    LOG_WARN("{}: prop guard {} -- flattening at {}", tag, d.reason, mark);
    broker->ClosePartial(1.0, mark, ts, "prop_guard");
  }
  return d;
}

// ObserveProp for the L2 path, where both sides of the book are in hand. a
// one-sided snapshot falls back to the mid, because returning cash on a
// broken quote (via GetEquityAtBidAsk's flat-position branch) would hide the
// exact open loss the guard exists to catch.
inline PropDecision ObservePropBook(PropGuard& guard, IBroker* broker,
                                    double bid, double ask, double mid,
                                    int64_t ts, const std::string& tag) {
  const double equity = (bid > 0.0 && ask > 0.0)
                            ? PaperEquityBidAsk(broker, bid, ask)
                            : PaperEquity(broker, mid);
  const PropDecision d = guard.Observe(equity, ts);

  if (d.should_flatten && mid > 0.0 && broker && broker->HasOpenPosition()) {
    LOG_WARN("{}: prop guard {} -- flattening at {}", tag, d.reason, mid);
    broker->ClosePartial(1.0, mid, ts, "prop_guard");
  }
  return d;
}

} // namespace mokuren
