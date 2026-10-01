#pragma once
#include "aggregator.hpp"
#include "ibroker.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

class PaperTrader : public IBroker {
public:
  PaperTrader(double sl_pct = 0.005, double tp_pct = 0.01, double max_dl = 400.0, double slip = 0.0002)
      : stop_loss_pct(sl_pct), take_profit_pct(tp_pct), max_daily_loss_(max_dl), slippage(slip) {}

  double GetBalance() const override { return balance; }
  bool HasOpenPosition() const override { return position_size > 0; }
  void UpdateStopLoss(double new_sl) override { current_sl_ = new_sl; }
  
  int GetTradesWon() const { return trades_won; }
  int GetTradesLost() const { return trades_lost; }
  int GetTradesBE() const { return trades_be; } 
  double GetTotalFeesPaid() const { return total_fees_paid; }
  int GetTotalTrades() const { return trades_won + trades_lost + trades_be; }
  double GetPositionSize() const { return position_size; }
  
  const std::vector<TradeRecord>& GetTradeHistory() const { return trade_history_; }
  double GetNetProfit() const { return balance - 10000.0; }
  double GetMaxDrawdown() const { return max_drawdown_pct_; }

  // inject config
  void ApplyManagementConfig(double be_trig, double be_targ, 
                             bool trail_en, double trail_trig, double trail_dist,
                             bool scale_en, double scale_trig, double scale_frac) {
    be_trigger_pct_ = be_trig;
    be_target_pct_  = be_targ;
    enable_trailing_ = trail_en;
    trailing_trigger_pct_ = trail_trig;
    trailing_dist_pct_ = trail_dist;
    enable_scale_out_ = scale_en;
    scale_out_trigger_pct_ = scale_trig;
    scale_out_fraction_ = scale_frac;
  }

  TradeSignal GetCurrentPosition() const override {
    TradeSignal sig;
    sig.direction = position_direction_;
    sig.volume = position_size;
    sig.entry_price = entry_price;
    sig.stop_loss = current_sl_;
    sig.take_profit = current_tp_;
    return sig;
  }

  double GetWinrate() const {
    double total_trades = GetTotalTrades();
    if (total_trades > 0) return (static_cast<double>(trades_won) / total_trades) * 100.0;
    return 0.0;
  }

  void CheckRisk(double current_price, int64_t current_time) {
    UpdateDay(current_time);
    if (position_size == 0.0) return;

    if (current_price > highest_seen_price_) highest_seen_price_ = current_price;
    if (current_price < lowest_seen_price_ || lowest_seen_price_ == 0.0) lowest_seen_price_ = current_price;

    double actual_exit_price = (position_direction_ == SignalDirection::BUY) ? (current_price * (1.0 - slippage)) : (current_price * (1.0 + slippage));
    double gross_profit = (position_direction_ == SignalDirection::BUY) ? (actual_exit_price - entry_price) * position_size : (entry_price - actual_exit_price) * position_size;
    double exit_fee = (actual_exit_price * position_size) * taker_fee_pct;
    double floating_net_profit = gross_profit - exit_fee; 

    UpdateDrawdown(balance + floating_net_profit);

    if (current_daily_pnl_ + floating_net_profit <= -max_daily_loss_) {
      ClosePosition(current_price, "daily_loss_limit", current_time);
      return;
    }

    if (enable_scale_out_ && !has_scaled_out_) {
        bool hit_scale_long = (position_direction_ == SignalDirection::BUY && current_price >= entry_price * (1.0 + scale_out_trigger_pct_));
        bool hit_scale_short = (position_direction_ == SignalDirection::SELL && current_price <= entry_price * (1.0 - scale_out_trigger_pct_));
        
        if (hit_scale_long || hit_scale_short) {
            ClosePartialPosition(current_price, scale_out_fraction_, "scale_out", current_time);
            has_scaled_out_ = true;
            current_sl_ = (position_direction_ == SignalDirection::BUY) ? entry_price * (1.0 + be_target_pct_) : entry_price * (1.0 - be_target_pct_);
            sl_moved_to_be_ = true;
            // log partial scale out stop move
            current_stop_history_.push_back({current_time, 0, current_sl_, "SCALE->BE"});
        }
    }

    if (position_direction_ == SignalDirection::BUY) {
      if (!sl_moved_to_be_ && current_price >= entry_price * (1.0 + be_trigger_pct_)) {
        current_sl_ = entry_price * (1.0 + be_target_pct_);
        sl_moved_to_be_ = true;
        // log breakeven hit
        current_stop_history_.push_back({current_time, 0, current_sl_, "BE TRIGGER"});
      }
      
      if (enable_trailing_ && current_price >= entry_price * (1.0 + trailing_trigger_pct_)) {
          double proposed_sl = highest_seen_price_ * (1.0 - trailing_dist_pct_);
          if (proposed_sl > current_sl_) {
              current_sl_ = proposed_sl; 
              // log trailing step
              current_stop_history_.push_back({current_time, 0, current_sl_, "TRAIL MOVE"});
          }
      }

      if (current_price <= current_sl_) ClosePosition(current_price, sl_moved_to_be_ ? "trail/be" : "sl", current_time);
      else if (current_price >= current_tp_) ClosePosition(current_price, "tp", current_time);
        
    } else if (position_direction_ == SignalDirection::SELL) {
      if (!sl_moved_to_be_ && current_price <= entry_price * (1.0 - be_trigger_pct_)) {
        current_sl_ = entry_price * (1.0 - be_target_pct_);
        sl_moved_to_be_ = true;
        // log breakeven hit
        current_stop_history_.push_back({current_time, 0, current_sl_, "BE TRIGGER"});
      }

      if (enable_trailing_ && current_price <= entry_price * (1.0 - trailing_trigger_pct_)) {
          double proposed_sl = lowest_seen_price_ * (1.0 + trailing_dist_pct_);
          if (proposed_sl < current_sl_ || current_sl_ == 0.0) {
              current_sl_ = proposed_sl; 
              // log trailing step
              current_stop_history_.push_back({current_time, 0, current_sl_, "TRAIL MOVE"});
          }
      }

      if (current_price >= current_sl_) ClosePosition(current_price, sl_moved_to_be_ ? "trail/be" : "sl", current_time);
      else if (current_price <= current_tp_) ClosePosition(current_price, "tp", current_time);
    }
  }

  void ProcessSignal(TradeSignal signal, double current_price, int64_t current_time) {
    UpdateDay(current_time);
    
    if (current_daily_pnl_ <= -max_daily_loss_) return;
    if (position_size > 0.0) return;

    if (signal.volume > 0.0 && (signal.direction == SignalDirection::BUY ||
                                signal.direction == SignalDirection::SELL)) {
      position_size = signal.volume;
      position_direction_ = signal.direction;
      entry_time_ = current_time;
      
      sl_moved_to_be_ = false;
      has_scaled_out_ = false;
      highest_seen_price_ = current_price;
      lowest_seen_price_ = current_price;
      current_stop_history_.clear(); // reset trailing log for new trade

      if (position_direction_ == SignalDirection::BUY) {
        entry_price = current_price * (1.0 + slippage);
        double sl_dist_pct = (signal.entry_price - signal.stop_loss) / signal.entry_price;
        double tp_dist_pct = (signal.take_profit - signal.entry_price) / signal.entry_price;
        current_sl_ = entry_price * (1.0 - sl_dist_pct);
        current_tp_ = entry_price * (1.0 + tp_dist_pct);
      } else {
        entry_price = current_price * (1.0 - slippage);
        double sl_dist_pct = (signal.stop_loss - signal.entry_price) / signal.entry_price;
        double tp_dist_pct = (signal.entry_price - signal.take_profit) / signal.entry_price;
        current_sl_ = entry_price * (1.0 + sl_dist_pct);
        current_tp_ = entry_price * (1.0 - tp_dist_pct);
      }

      double entry_fee = (entry_price * position_size) * taker_fee_pct;
      current_entry_fee_ = entry_fee; 
      balance -= entry_fee;
      total_fees_paid += entry_fee;
      current_daily_pnl_ -= entry_fee; 
      
      UpdateDrawdown(balance);
    }
  }

  void CloseOpenPositionAtEnd(double final_price, int64_t end_time) {
    ClosePosition(final_price, "end of backtest", end_time);
  }

private:
  double balance = 10000.0;
  double position_size = 0.0;
  double entry_price = 0.0;
  
  double peak_balance_ = 10000.0;
  double max_drawdown_pct_ = 0.0;
  
  int trades_won = 0;
  int trades_lost = 0;
  int trades_be = 0; 

  SignalDirection position_direction_ = SignalDirection::NONE;
  double current_sl_ = 0.0;
  double current_tp_ = 0.0;
  int64_t entry_time_ = 0;
  double current_entry_fee_ = 0.0; 
  
  bool sl_moved_to_be_ = false;
  double be_trigger_pct_ = 0.005; 
  double be_target_pct_  = 0.001; 

  bool enable_trailing_ = true;
  double trailing_trigger_pct_ = 0.008; 
  double trailing_dist_pct_ = 0.004;    
  double highest_seen_price_ = 0.0;
  double lowest_seen_price_ = 0.0;

  bool enable_scale_out_ = false;       
  bool has_scaled_out_ = false;
  double scale_out_trigger_pct_ = 0.006;
  double scale_out_fraction_ = 0.5;     

  double max_daily_loss_ = 400.0;
  double current_daily_pnl_ = 0.0;
  int64_t current_day_start_ = 0;

  std::vector<TradeRecord> trade_history_;
  std::vector<StopEvent> current_stop_history_; // local event collector

  double stop_loss_pct;
  double take_profit_pct;
  double taker_fee_pct = 0.0004;
  double slippage;         
  double total_fees_paid = 0.0;  

  void UpdateDrawdown(double current_equity) {
    if (current_equity > peak_balance_) peak_balance_ = current_equity;
    double current_dd = (peak_balance_ - current_equity) / peak_balance_ * 100.0;
    if (current_dd > max_drawdown_pct_) max_drawdown_pct_ = current_dd;
  }

  void UpdateDay(int64_t current_time) {
    int64_t day_ms = 86400000; 
    int64_t new_day = current_time - (current_time % day_ms);
    
    if (current_day_start_ == 0) {
      current_day_start_ = new_day;
    } else if (new_day > current_day_start_) {
      current_day_start_ = new_day;
      current_daily_pnl_ = 0.0; 
    }
  }

  void ClosePartialPosition(double current_price, double fraction, const std::string &reason, int64_t current_time) {
      if (position_size <= 0.0) return;
      
      double close_volume = position_size * fraction;
      double actual_exit_price, gross_profit;

      if (position_direction_ == SignalDirection::BUY) {
        actual_exit_price = current_price * (1.0 - slippage);
        gross_profit = (actual_exit_price - entry_price) * close_volume;
      } else {
        actual_exit_price = current_price * (1.0 + slippage);
        gross_profit = (entry_price - actual_exit_price) * close_volume;
      }

      double exit_fee = (actual_exit_price * close_volume) * taker_fee_pct;
      total_fees_paid += exit_fee;
      double net_profit = gross_profit - exit_fee;
      
      balance += net_profit;
      current_daily_pnl_ += net_profit; 
      UpdateDrawdown(balance);

      double partial_entry_fee = current_entry_fee_ * fraction;
      current_entry_fee_ -= partial_entry_fee;
      double true_trade_pnl = net_profit - partial_entry_fee;

      // EXPLICIT STRUCT INSTANTIATION TO PREVENT C++ BRACE-ENCLOSURE ERRORS
      TradeRecord rec;
      rec.entry_time = entry_time_;
      rec.exit_time = current_time;
      rec.direction = position_direction_;
      rec.entry_price = entry_price;
      rec.exit_price = actual_exit_price;
      rec.net_profit = true_trade_pnl;
      rec.exit_reason = reason;
      rec.stop_events = current_stop_history_;

      // append partial close to trade history
      trade_history_.push_back(rec);
      
      position_size -= close_volume; 
  }

  void ClosePosition(double current_price, const std::string &reason, int64_t current_time) {
    if (position_size > 0.0) {
      double actual_exit_price;
      double gross_profit;

      if (position_direction_ == SignalDirection::BUY) {
        actual_exit_price = current_price * (1.0 - slippage);
        gross_profit = (actual_exit_price - entry_price) * position_size;
      } else {
        actual_exit_price = current_price * (1.0 + slippage);
        gross_profit = (entry_price - actual_exit_price) * position_size;
      }

      double exit_fee = (actual_exit_price * position_size) * taker_fee_pct;
      total_fees_paid += exit_fee;
      double net_profit = gross_profit - exit_fee;
      
      balance += net_profit;
      current_daily_pnl_ += net_profit; 
      
      UpdateDrawdown(balance);

      if (net_profit > 0) trades_won++;
      else if (net_profit < 0) trades_lost++;
      else trades_be++;

      double true_trade_pnl = net_profit - current_entry_fee_;

      // EXPLICIT STRUCT INSTANTIATION TO PREVENT C++ BRACE-ENCLOSURE ERRORS
      TradeRecord rec;
      rec.entry_time = entry_time_;
      rec.exit_time = current_time;
      rec.direction = position_direction_;
      rec.entry_price = entry_price;
      rec.exit_price = actual_exit_price;
      rec.net_profit = true_trade_pnl;
      rec.exit_reason = reason;
      rec.stop_events = current_stop_history_;

      // append final close to trade history
      trade_history_.push_back(rec);

      position_size = 0.0;
      entry_price = 0.0;
      current_sl_ = 0.0;
      current_tp_ = 0.0;
      entry_time_ = 0;
      current_entry_fee_ = 0.0; 
      sl_moved_to_be_ = false;
      has_scaled_out_ = false;
      position_direction_ = SignalDirection::NONE;
      current_stop_history_.clear(); // reset trailing log 
    }
  }
};