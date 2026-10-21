#pragma once

#include "ibroker.hpp"
#include "logger.hpp"
#include "prop_guard.hpp"

#include <cmath>
#include <string>

namespace mokuren {

// Shared per-run prop-guard plumbing for the backtest, WFA and replay paths.
//
// The live orchestrator builds equity from the venue's mark-to-market balance
// (or, on paper, closed balance plus the floating leg of the open position)
// and feeds it to PropGuard::Observe on every print. The backtest paths have
// no venue, so they measure the same thing from PaperTrader: closed balance
// plus the floating leg of the open position. This header is the one place
// that derivation lives, so scan, WFA-IS, WFA-OOS and replay cannot drift.
//
// A fresh PropGuard starts Unarmed and refuses entries until the first
// Observe() with a positive equity reading. Callers must therefore call
// ObserveProp() before the first signal gate -- not just periodically -- or
// the first trade's signals are silently dropped.
inline double PaperEquity(IBroker* broker, double mark) {
  if (!broker) return 0.0;
  double equity = broker->GetBalance();
  if (mark > 0.0) {
    const TradeSignal pos = broker->GetCurrentPosition();
    if (pos.volume > 0.0 && pos.entry_price > 0.0 &&
        pos.direction != SignalDirection::NONE) {
      const double dir = pos.direction == SignalDirection::BUY ? 1.0 : -1.0;
      equity += (mark - pos.entry_price) * pos.volume * dir;
    }
  }
  return equity;
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

} // namespace mokuren
