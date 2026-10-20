#pragma once
#include "ibroker.hpp"
#include <cmath>
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

  void Update(double current_price, int64_t current_time) {
    if (!enabled_) return;

    // reset state if no active trade
    if (!broker_->HasOpenPosition()) {
      stop_moved_to_be_ = false;
      scaled_out_ = false;
      return;
    }

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

    // secure profit and protect the trade
    if (!stop_moved_to_be_ && profit >= be_trigger_r_ * risk_distance) {
      const double be_stop =
          is_long ? pos.entry_price + be_target_r_ * risk_distance
                  : pos.entry_price - be_target_r_ * risk_distance;
      broker_->UpdateStopLoss(be_stop);
      stop_moved_to_be_ = true;
    }

    // ratchet the stop behind price once the trailing trigger is reached.
    if (enable_trailing_ && profit >= trailing_trigger_r_ * risk_distance) {
      const double trail_stop =
          is_long ? current_price - trailing_dist_r_ * risk_distance
                  : current_price + trailing_dist_r_ * risk_distance;
      // never move the stop backwards.
      if ((is_long && trail_stop > pos.stop_loss) ||
          (!is_long && trail_stop < pos.stop_loss)) {
        broker_->UpdateStopLoss(trail_stop);
      }
    }
  }
};
