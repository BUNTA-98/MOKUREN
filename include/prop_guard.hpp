#pragma once

#include <cstdint>

namespace mokuren {

// A funded account's survival rules. Both limits share one denominator, and
// it is the INITIAL balance, not current equity: the daily floor is
// day_start_equity - (initial_balance * pct), i.e. a fixed dollar band that
// does not widen when the account runs up. These are the firm's numbers, not
// the strategy's, so they live outside both: one guard, evaluated the same way
// on the paper, replay and live paths, instead of a third risk ladder hidden
// inside a venue adapter.
struct PropRules {
  // default off so a directly-constructed PropRules (tests, tooling) is inert
  // until asked. AppConfig::Load arms this whenever the config carries a "prop"
  // block, so a backtest that writes one enforces the firm's limits for real:
  // it no longer needs a separate "enabled": true to take effect, and a run
  // that means "no account limits" says so by dropping the block entirely.
  bool enabled = false;

  // hyrotrader one-step: 4% daily (3% zero-step, 5% two-step), 6% overall.
  // fractions, not percent, and of the INITIAL balance in both cases: up 10%
  // today still leaves the same dollar band of daily room, and the band does
  // not shrink after a red day either. the plan tier is the only thing that
  // moves these -- changing account type means changing them explicitly.
  double max_daily_drawdown_pct = 0.04;
  double max_total_drawdown_pct = 0.06;

  // stop taking new risk at this fraction of either limit. the hard limit
  // stays the backstop; this is what keeps a bad day from becoming a breach.
  double warn_fraction = 0.8;

  // the firm's day boundary, in whole hours from utc midnight. 0 = utc.
  int daily_reset_hour_utc = 0;

  // realise the position once a limit is breached. a breach is terminal with
  // most firms, so this is a policy choice rather than a safety improvement.
  bool flatten_on_breach = true;

  // NOTE: no starting balance here. the base total drawdown is measured
  // against is ACCOUNT STATE, not policy, so it is passed to PropGuard
  // explicitly and never stored in the rules. keeping it in both places is how
  // the guard ended up measuring against a number the account never had: the
  // paper account was seeded from initial_balance while the guard kept its own
  // value (or 0), so total_drawdown() divided by a different base.
};

enum class PropState {
  Unarmed,     // no usable equity yet: refuse entries rather than trade blind
  Normal,
  Warning,     // near a limit: no new entries, keep managing what is open
  DailyHalted, // daily limit hit: no new entries until the day rolls over
  TotalHalted, // total limit hit: no new entries for the rest of the process
};

struct PropDecision {
  PropState state = PropState::Unarmed;
  bool allow_new_entry = false;
  bool should_flatten = false; // true only on the transition into a breach
  const char* reason = "guard unarmed";
};

// Pure policy: no clock, no socket, no logger. The caller supplies equity and
// time and acts on the decision, which keeps the rules unit-testable and keeps
// the orchestrator the only owner of "what to do about it".
class PropGuard {
public:
  // starting_equity is the base total drawdown is measured against -- the same
  // number that funds the account:
  //   backtest / scan / wfa / replay -> cfg.initial_balance, which also seeds
  //     PaperTrader, so the two cannot disagree
  //   live -> the firm's evaluation baseline, NOT the current venue balance,
  //     which already reflects losses taken before this process started
  //
  // > 0 arms the guard immediately: drawdown is measurable from tick 0, so the
  // "a fresh guard starts Unarmed and refuses entries until the first
  // Observe()" ordering trap disappears. 0 keeps the legacy behaviour and
  // derives the base from the first positive equity reading, which is only
  // right when a venue has not delivered a balance yet.
  explicit PropGuard(PropRules rules = PropRules{}, double starting_equity = 0.0);

  // equity is mark-to-market account equity, including open pnl -- that is
  // what the firm measures. safe for the hot path: a few comparisons.
  PropDecision Observe(double equity, int64_t now_ms);

  // clear per-run latches (call from Start so a restarted orchestrator does
  // not inherit the previous run's halt).
  void Reset();

  PropState state() const { return state_; }
  bool allow_new_entry() const { return allow_new_entry_; }

  double initial_equity() const { return initial_equity_; }
  double day_start_equity() const { return day_start_equity_; }
  double peak_equity() const { return peak_equity_; }
  // both are fractions of the INITIAL balance, so both compare directly
  // against the limits above. daily_* is the day's loss measured from
  // day_start_equity_; total_* is measured from initial_equity_. dividing
  // daily_* by day_start_equity_ instead would be wrong in both directions
  // (wider band when up, narrower when down). 0 if disabled or unarmed.
  double daily_drawdown() const;
  double total_drawdown() const;

private:
  void RollDay(int64_t now_ms, double equity);

  PropRules rules_;
  // remembered so Reset() can restore the armed state without the caller having
  // to re-supply a base it already gave the constructor.
  double starting_equity_ = 0.0;
  PropState state_ = PropState::Unarmed;
  bool allow_new_entry_ = false;
  bool total_breached_ = false; // latched: never clears within the process
  bool daily_breached_ = false; // clears when the day rolls over

  bool have_day_ = false;
  int64_t day_index_ = 0;
  double initial_equity_ = 0.0;
  double day_start_equity_ = 0.0;
  double peak_equity_ = 0.0;
  double last_equity_ = 0.0;
};

} // namespace mokuren
