#pragma once
#include "ibroker.hpp"
#include "logger.hpp"
#include "market_context.hpp"
#include "structural_exit.hpp"
#include <cmath>
#include <cstdint>
#include <string>

// handles active trade management, scale outs and trailing stops
class PositionManager {
private:
  IBroker* broker_;
  bool stop_moved_to_be_ = false;
  bool scaled_out_ = false;
  
  bool enable_scale_out_; // toggle for partial takes
  double scale_fraction_; // fraction to sell (e.g. 0.7 for 70%)
  bool enabled_ = true;   // false when the broker manages the trade itself

  // Third close path on an open position: market structure / trade age / MFE.
  // Owned here so it shares the position state (entry_ts, mfe, mae) with the
  // BE/trail/scale ladder, and so a structural close and a BE/trail/scale
  // close cannot race on the same tick. ConfigureStructuralExit is called
  // once at construction time (factory or runner_common); absent config => the
  // guard is inert and this file behaves exactly as before.
  mokuren::StructuralExitGuard structural_exit_;

  // Thresholds are multiples of the placed risk (1R = |entry - stop|), never
  // percentages of entry. The defaults reproduce the old hardcoded behaviour
  // (break-even and one scale-out at 1R); Configure() replaces them with the
  // r-multiples the config actually names.
  double be_trigger_r_ = 1.0;
  double be_target_r_ = 0.0;
  bool enable_trailing_ = false;
  double trailing_trigger_r_ = 0.0;
  double trailing_dist_r_ = 0.0;
  double scale_out_trigger_r_ = 1.0;

  // a venue may reject a stop move (off-tick, rate limit, transient reject),
  // and bybit's UpdateStopLoss deliberately does not commit local state in
  // that case. without a cooldown the retry would fire on every tick; with
  // one it fires at most once a second, which a minute-bar strategy can
  // absorb without spamming the venue.
  int64_t last_stop_attempt_ms_ = 0;
  static constexpr int64_t kStopRetryCooldownMs = 1000;

  // trailing has its own clock and its own gates. the break-even branch
  // latches stop_moved_to_be_ on success, but the trailing branch recomputes
  // the same trail on every tick once it is active; without a cooldown and a
  // minimum step, that is a REST call per tick. the cooldown bounds the rate,
  // the minimum step suppresses no-op moves (a trail that would land within
  // 0.1R of the current stop is not worth a round-trip).
  int64_t last_trailing_attempt_ms_ = 0;
  static constexpr int64_t kTrailingCooldownMs = 5000;
  static constexpr double kTrailingMinStepR = 0.1;

  bool CanRetryStopMove(int64_t now_ms) const {
    if (last_stop_attempt_ms_ == 0) return true;
    return now_ms - last_stop_attempt_ms_ >= kStopRetryCooldownMs;
  }

  bool CanRetryTrailing(int64_t now_ms) const {
    if (last_trailing_attempt_ms_ == 0) return true;
    return now_ms - last_trailing_attempt_ms_ >= kTrailingCooldownMs;
  }

public:
  PositionManager(IBroker* b, bool enable_scale = false, double scale_fraction = 0.7) 
      : broker_(b), enable_scale_out_(enable_scale), scale_fraction_(scale_fraction) {}

  // r-multiples from the trade_management block. called once, after the config
  // is loaded, so the ladder follows the placed stop instead of a percentage
  // derived from risk.sl_pct.
  void Configure(double be_trigger_r, double be_target_r,
                 bool enable_trailing, double trailing_trigger_r,
                 double trailing_dist_r, bool enable_scale_out,
                 double scale_out_trigger_r, double scale_fraction) {
    be_trigger_r_ = be_trigger_r;
    be_target_r_ = be_target_r;
    enable_trailing_ = enable_trailing;
    trailing_trigger_r_ = trailing_trigger_r;
    trailing_dist_r_ = trailing_dist_r;
    enable_scale_out_ = enable_scale_out;
    scale_out_trigger_r_ = scale_out_trigger_r;
    scale_fraction_ = scale_fraction;
  }

  // PaperTrader applies break-even/scale-out from the trade_management block,
  // so running this manager on top fired both twice. callers disable it for
  // brokers that already manage their own position.
  void SetEnabled(bool on) { enabled_ = on; }
  bool IsEnabled() const { return enabled_; }

  // Arm the structural exit guard. Called once, after the config is loaded.
  // A disabled StructuralExitConfig leaves the guard inert; no other code has
  // to check the master switch.
  void ConfigureStructuralExit(const mokuren::StructuralExitConfig& c) {
    structural_exit_.Configure(c);
  }

  // The MarketContext is passed on the trade tick so structural-exit rules
  // that read the bar / book see the same state alpha sees. The pre-existing
  // BE/trail/scale body does not use it -- those branches key on current_price
  // only -- so the signature change is behaviour-neutral for the ladder.
  void Update(const MarketContext& ctx, double current_price, int64_t current_time) {
    // reset state if no active trade. Done BEFORE the enabled_ early return so
    // a SetEnabled(false) or a structural-only config still leaves the guard
    // clean for the next position.
    if (!broker_->HasOpenPosition()) {
      stop_moved_to_be_ = false;
      scaled_out_ = false;
      last_stop_attempt_ms_ = 0;
      last_trailing_attempt_ms_ = 0;
      structural_exit_.ResetTradeState();
      return;
    }

    // Structural exits run on the trade tick regardless of enabled_: they are
    // their own close path, with their own master switch (structural_exit.
    // enabled). When the guard fires, the position is flat before BE/trail/
    // scale can race it; the next tick sees HasOpenPosition()==false and
    // resets. Checking HasOpenPosition() again after the call is what makes
    // that ordering real -- a fired structural close must not be followed by
    // a scale_out or a stop move against a position that no longer exists.
    structural_exit_.OnTrade(ctx, broker_, current_price, current_time);
    if (!broker_->HasOpenPosition()) return;

    if (!enabled_) return;

    TradeSignal pos = broker_->GetCurrentPosition();
    if (pos.direction == SignalDirection::NONE || pos.stop_loss == 0.0) return;

    // 1R is the placed stop: the risk actually taken. measuring it from the
    // config's risk.sl_pct would leave the ladder anchored to a number that
    // stops matching the trade as soon as a structural stop is used.
    const double risk_distance = std::abs(pos.entry_price - pos.stop_loss);
    if (!(risk_distance > 0.0)) return;

    const bool is_long = pos.direction == SignalDirection::BUY;
    const double profit = is_long ? current_price - pos.entry_price
                                  : pos.entry_price - current_price;

    // optional partial scale out
    if (enable_scale_out_ && !scaled_out_ &&
        profit >= scale_out_trigger_r_ * risk_distance) {
      broker_->ClosePartial(scale_fraction_, current_price, current_time,
                            "scale_out");
      scaled_out_ = true;
    }

    // secure profit and protect the trade.
    //
    // confirm the write actually landed. a venue that rejected the move
    // leaves the broker's stop at the previous value; latching
    // stop_moved_to_be_ here regardless is how a break-even went missing on
    // a prop account without a single warning. retry on a cooldown rather
    // than per-tick, so a persistent reject cannot turn into a rate-limit
    // ban (or a blocked worker thread).
    if (!stop_moved_to_be_ && profit >= be_trigger_r_ * risk_distance) {
      const double be_stop =
          is_long ? pos.entry_price + be_target_r_ * risk_distance
                  : pos.entry_price - be_target_r_ * risk_distance;
      if (CanRetryStopMove(current_time)) {
        const double before = pos.stop_loss;
        broker_->UpdateStopLoss(be_stop);
        const double after = broker_->GetCurrentPosition().stop_loss;
        if (std::abs(after - before) > 1e-12) {
          stop_moved_to_be_ = true;
          // the trailing branch below compares against pos.stop_loss, which
          // is the value read at the top of this call. refresh it so a
          // break-even that just landed is not immediately re-sent as a
          // trail move to the same price.
          pos.stop_loss = after;
        } else {
          last_stop_attempt_ms_ = current_time;
          LOG_WARN("PositionManager: break-even move to {} did not take "
                   "(broker stop still {}); will retry",
                   be_stop, before);
        }
      }
    }

    // ratchet the stop behind price once the trailing trigger is reached.
    // two gates keep this from becoming a REST call per tick:
    //   - kTrailingMinStepR: a trail that would land within 0.1R of the
    //     current stop is not worth a round-trip.
    //   - kTrailingCooldownMs: at most one trail move every 5 seconds.
    if (enable_trailing_ && profit >= trailing_trigger_r_ * risk_distance) {
      const double trail_stop =
          is_long ? current_price - trailing_dist_r_ * risk_distance
                  : current_price + trailing_dist_r_ * risk_distance;
      // never move the stop backwards.
      const bool improves =
          (is_long && trail_stop > pos.stop_loss) ||
          (!is_long && trail_stop < pos.stop_loss);
      const bool meaningful =
          std::abs(trail_stop - pos.stop_loss) >=
          kTrailingMinStepR * risk_distance;
      if (improves && meaningful && CanRetryTrailing(current_time)) {
        const double before = pos.stop_loss;
        broker_->UpdateStopLoss(trail_stop);
        const double after = broker_->GetCurrentPosition().stop_loss;
        // record the attempt whether it landed or not: a persistent reject
        // must not turn into a per-tick retry storm, and a landed move must
        // not be re-sent on the next tick.
        last_trailing_attempt_ms_ = current_time;
        if (std::abs(after - before) <= 1e-12) {
          LOG_WARN("PositionManager: trail move to {} did not take "
                   "(broker stop still {}); will retry after cooldown",
                   trail_stop, before);
        }
      }
    }
  }

  // Book-path entry point: BE/trail/scale are trade-price driven and stay on
  // Update (above); structural rules that watch the book fire here. Separate
  // method on purpose: calling Update itself from a book frame would trigger
  // BE/trail on a book price that has no trade behind it, which is what the
  // current design deliberately avoids.
  void UpdateOnL2(const MarketContext& ctx, int64_t current_time) {
    if (!broker_) return;
    if (!broker_->HasOpenPosition()) {
      structural_exit_.ResetTradeState();
      return;
    }
    structural_exit_.OnBook(ctx, broker_, current_time);
  }
};
