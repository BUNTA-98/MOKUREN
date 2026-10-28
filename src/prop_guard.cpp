#include "prop_guard.hpp"

#include <cmath>

namespace mokuren {
namespace {

// floor division, so a day index below the epoch still buckets correctly.
int64_t FloorDiv(int64_t a, int64_t b) {
  const int64_t q = a / b;
  const int64_t r = a % b;
  return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

} // namespace

PropGuard::PropGuard(PropRules rules, double starting_equity)
    : rules_(rules), starting_equity_(starting_equity) {
  if (std::isfinite(starting_equity) && starting_equity > 0.0) {
    // Seed every anchor, not just initial_equity_. daily_drawdown() divides by
    // day_start_equity_ and subtracts last_equity_, so leaving those at 0 would
    // report a 100% drawdown on a freshly funded account.
    initial_equity_ = starting_equity;
    day_start_equity_ = starting_equity;
    peak_equity_ = starting_equity;
    last_equity_ = starting_equity;
    state_ = PropState::Normal;
    allow_new_entry_ = true;
  }
}

void PropGuard::Reset() {
  total_breached_ = false;
  daily_breached_ = false;
  have_day_ = false;
  day_index_ = 0;
  // restore the base the caller supplied, not zero: a restarted orchestrator
  // that reset to 0 would derive its baseline from the first frame it sees,
  // which on a live restart is already drawn down.
  initial_equity_ = starting_equity_;
  day_start_equity_ = starting_equity_;
  peak_equity_ = starting_equity_;
  last_equity_ = starting_equity_;
  state_ = starting_equity_ > 0.0 ? PropState::Normal : PropState::Unarmed;
  allow_new_entry_ = starting_equity_ > 0.0;
}

double PropGuard::daily_drawdown() const {
  // a disabled guard has no limits in effect, so a percentage is meaningless:
  // Observe() returns before anchoring anything, leaving last_equity_ at zero,
  // and (initial - last) / initial then reads as a total loss. nothing can
  // block or size here, but "total_dd=100%" on a healthy shadow run is exactly
  // the line that gets misread during an incident.
  if (!rules_.enabled) return 0.0;
  // The firm's daily floor is day_start_equity - (initial_balance * pct): a
  // fixed dollar band, not a fixed percentage of today's opening equity.
  // Expressing the day's loss as a fraction of the *initial* balance is what
  // makes the comparison against max_daily_drawdown_pct mean the same thing
  // on every day. Dividing by day_start_equity_ instead widens the band on a
  // green day (up 10% -> 4.4% of initial) and narrows it on a red one, so the
  // guard would be most lenient exactly when the account is furthest above
  // its baseline.
  if (!(initial_equity_ > 0.0)) return 0.0;
  return (day_start_equity_ - last_equity_) / initial_equity_;
}

double PropGuard::total_drawdown() const {
  if (!rules_.enabled) return 0.0;
  if (!(initial_equity_ > 0.0)) return 0.0;
  return (initial_equity_ - last_equity_) / initial_equity_;
}

void PropGuard::RollDay(int64_t now_ms, double equity) {
  const int64_t boundary =
      static_cast<int64_t>(rules_.daily_reset_hour_utc) * 3600000LL;
  const int64_t day = FloorDiv(now_ms - boundary, 86400000LL);

  if (!have_day_) {
    have_day_ = true;
    day_index_ = day;
    day_start_equity_ = equity;
    return;
  }
  if (day > day_index_) {
    day_index_ = day;
    // the firm resets against the equity it sees at the boundary, not against
    // yesterday's start, so re-anchor on the reading that crosses it.
    day_start_equity_ = equity;
    daily_breached_ = false;
  }
}

PropDecision PropGuard::Observe(double equity, int64_t now_ms) {
  PropDecision d;

  if (!rules_.enabled) {
    state_ = PropState::Normal;
    allow_new_entry_ = true;
    d.state = state_;
    d.allow_new_entry = true;
    d.reason = "guard disabled";
    return d;
  }

  // a terminal breach outlives any later reading, including a stale or empty
  // wallet frame that arrives after the loss was realised.
  if (total_breached_) {
    state_ = PropState::TotalHalted;
    allow_new_entry_ = false;
    d.state = state_;
    d.reason = "total drawdown limit breached";
    return d;
  }

  if (!std::isfinite(equity) || equity <= 0.0) {
    // no usable mark yet (live: the wallet frame has not landed). refuse new
    // risk instead of trading against a limit we cannot measure.
    state_ = PropState::Unarmed;
    allow_new_entry_ = false;
    d.state = state_;
    d.reason = "guard unarmed: no usable equity";
    return d;
  }

  if (!(initial_equity_ > 0.0)) initial_equity_ = equity;
  RollDay(now_ms, equity);

  last_equity_ = equity;
  if (equity > peak_equity_) peak_equity_ = equity;

  const bool was_daily = daily_breached_;
  const bool was_total = total_breached_;

  const double daily_limit = rules_.max_daily_drawdown_pct;
  const double total_limit = rules_.max_total_drawdown_pct;
  const double dd_daily = daily_drawdown();
  const double dd_total = total_drawdown();

  if (daily_limit > 0.0 && dd_daily >= daily_limit) daily_breached_ = true;
  if (total_limit > 0.0 && dd_total >= total_limit) total_breached_ = true;

  if (total_breached_) {
    state_ = PropState::TotalHalted;
  } else if (daily_breached_) {
    state_ = PropState::DailyHalted;
  } else if ((daily_limit > 0.0 &&
              dd_daily >= daily_limit * rules_.warn_fraction) ||
             (total_limit > 0.0 &&
              dd_total >= total_limit * rules_.warn_fraction)) {
    state_ = PropState::Warning;
  } else {
    state_ = PropState::Normal;
  }

  allow_new_entry_ = (state_ == PropState::Normal);

  d.state = state_;
  d.allow_new_entry = allow_new_entry_;
  // flatten once, on the transition. repeating it every tick would spray
  // reduce-only orders at a venue that already closed the position.
  d.should_flatten = rules_.flatten_on_breach &&
                     ((total_breached_ && !was_total) ||
                      (daily_breached_ && !was_daily));

  switch (state_) {
    case PropState::TotalHalted:
      d.reason = "total drawdown limit breached";
      break;
    case PropState::DailyHalted:
      d.reason = "daily drawdown limit breached";
      break;
    case PropState::Warning:
      d.reason = "approaching a drawdown limit";
      break;
    case PropState::Normal:
      d.reason = "ok";
      break;
    case PropState::Unarmed:
      d.reason = "guard unarmed";
      break;
  }
  return d;
}

} // namespace mokuren
