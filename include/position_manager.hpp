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

public:
  PositionManager(IBroker* b, bool enable_scale = false, double scale_fraction = 0.7) 
      : broker_(b), enable_scale_out_(enable_scale), scale_fraction_(scale_fraction) {}

  void Update(double current_price, int64_t current_time) {
    // reset state if no active trade
    if (!broker_->HasOpenPosition()) {
      stop_moved_to_be_ = false;
      scaled_out_ = false;
      return;
    }

    TradeSignal pos = broker_->GetCurrentPosition();
    if (pos.direction == SignalDirection::NONE || pos.stop_loss == 0.0) return;

    // calculate 1r risk distance (entry to structural stop)
    double risk_distance = std::abs(pos.entry_price - pos.stop_loss);
    
    bool hit_1r = false;
    if (pos.direction == SignalDirection::BUY && current_price >= pos.entry_price + risk_distance) hit_1r = true;
    if (pos.direction == SignalDirection::SELL && current_price <= pos.entry_price - risk_distance) hit_1r = true;

    // secure profit and protect the trade
    if (hit_1r && !stop_moved_to_be_) {
      
      // optional partial scale out
      if (enable_scale_out_ && !scaled_out_) {
        broker_->ClosePartial(scale_fraction_, current_price, current_time, "scale_out_1r");
        scaled_out_ = true;
      }
      
      // always pull stop to break-even at 1r
      broker_->UpdateStopLoss(pos.entry_price);
      stop_moved_to_be_ = true;
    }
  }
};