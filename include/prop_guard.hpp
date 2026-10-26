#pragma once

#include <cstdint>

namespace mokuren {

// A funded account's survival rules, expressed as fractions of equity. These
// are the firm's numbers, not the strategy's, so they live outside both: one
// guard, evaluated the same way on the paper, replay and live paths, instead
// of a third risk ladder hidden inside a venue adapter.
struct PropRules {
  bool enabled = false;

  // hyrotrader: 4% daily, 6% overall. fractions, not percent.
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
  double daily_drawdown() const; // fraction of the day's start, 0 if unarmed
  double total_drawdown() const; // fraction of the initial balance

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
