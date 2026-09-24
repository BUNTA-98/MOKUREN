#pragma once
#include "aggregator.hpp"
#include <iostream>
#include <string>
#include <vector>

struct TradeRecord {
  int64_t entry_time;
  int64_t exit_time;
  SignalDirection direction;
  double entry_price;
  double exit_price;
  double net_profit;
  std::string exit_reason;
};

class PaperTrader {
public:
  // NEU: max_dl = 400.0 (Prop Firm Not-Aus Grenze pro Tag)
  PaperTrader(double sl_pct = 0.005, double tp_pct = 0.01, double max_dl = 400.0)
      : stop_loss_pct(sl_pct), take_profit_pct(tp_pct), max_daily_loss_(max_dl) {}

  double GetBalance() const { return balance; }
  int GetTradesWon() const { return trades_won; }
  int GetTradesLost() const { return trades_lost; }
  double GetTotalFeesPaid() const { return total_fees_paid; }
  int GetTotalTrades() const { return trades_won + trades_lost; }
  double GetPositionSize() const { return position_size; }
  void UpdateStopLoss(double new_sl) { current_sl_ = new_sl; }
  bool HasOpenPosition() const { return position_size > 0; }
  const std::vector<TradeRecord>& GetTradeHistory() const { return trade_history_; }
  double GetNetProfit() const { return balance - 10000.0; }

  TradeSignal GetCurrentPosition() const {
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
    // 1. Tagessprung prüfen
    UpdateDay(current_time);

    if (position_size == 0.0) return;

    // 2. Prop Firm Floating PnL Überwachung
    double actual_exit_price = (position_direction_ == SignalDirection::BUY) ? (current_price - slippage) : (current_price + slippage);
    double gross_profit = (position_direction_ == SignalDirection::BUY) ? (actual_exit_price - entry_price) * position_size : (entry_price - actual_exit_price) * position_size;
    double exit_fee = (actual_exit_price * position_size) * taker_fee_pct;
    double floating_net_profit = gross_profit - exit_fee; 

    // Wenn offener + bereits geschlossener Verlust das Limit reißen -> Not-Aus!
    if (current_daily_pnl_ + floating_net_profit <= -max_daily_loss_) {
      ClosePosition(current_price, "daily_loss_limit", current_time);
      return;
    }

    // 3. Break-Even und Standard Risk Management
    if (position_direction_ == SignalDirection::BUY) {
      if (!sl_moved_to_be_ && current_price >= entry_price * (1.0 + be_trigger_pct_)) {
        current_sl_ = entry_price * (1.0 + be_target_pct_);
        sl_moved_to_be_ = true;
      }
      if (current_price <= current_sl_) ClosePosition(current_price, sl_moved_to_be_ ? "be" : "sl", current_time);
      else if (current_price >= current_tp_) ClosePosition(current_price, "tp", current_time);
        
    } else if (position_direction_ == SignalDirection::SELL) {
      if (!sl_moved_to_be_ && current_price <= entry_price * (1.0 - be_trigger_pct_)) {
        current_sl_ = entry_price * (1.0 - be_target_pct_);
        sl_moved_to_be_ = true;
      }
      if (current_price >= current_sl_) ClosePosition(current_price, sl_moved_to_be_ ? "be" : "sl", current_time);
      else if (current_price <= current_tp_) ClosePosition(current_price, "tp", current_time);
    }
  }

  void ProcessSignal(TradeSignal signal, double current_price, int64_t current_time) {
    UpdateDay(current_time);

    // Prop-Firm Lock: Hat der Bot heute schon 400$ verloren, verbieten wir neue Trades komplett
    if (current_daily_pnl_ <= -max_daily_loss_) return;

    if (position_size > 0.0) return;

    if (signal.volume > 0.0 && (signal.direction == SignalDirection::BUY ||
                                signal.direction == SignalDirection::SELL)) {
      position_size = signal.volume;
      position_direction_ = signal.direction;
      current_sl_ = signal.stop_loss;
      current_tp_ = signal.take_profit;
      entry_time_ = current_time;
      sl_moved_to_be_ = false;

      if (position_direction_ == SignalDirection::BUY) entry_price = current_price + slippage;
      else entry_price = current_price - slippage;

      double entry_fee = (entry_price * position_size) * taker_fee_pct;
      balance -= entry_fee;
      total_fees_paid += entry_fee;
      
      // Die Einstiegsgebühr drückt den Tages-PnL sofort ins Minus
      current_daily_pnl_ -= entry_fee; 
    }
  }

  void CloseOpenPositionAtEnd(double final_price, int64_t end_time) {
    ClosePosition(final_price, "end of backtest", end_time);
  }

private:
  double balance = 10000.0;
  double position_size = 0.0;
  double entry_price = 0.0;
  int trades_won = 0;
  int trades_lost = 0;

  SignalDirection position_direction_ = SignalDirection::NONE;
  double current_sl_ = 0.0;
  double current_tp_ = 0.0;
  int64_t entry_time_ = 0;
  
  bool sl_moved_to_be_ = false;
  double be_trigger_pct_ = 0.005; 
  double be_target_pct_  = 0.001; 

  // NEU: Prop Firm Variablen
  double max_daily_loss_ = 400.0;
  double current_daily_pnl_ = 0.0;
  int64_t current_day_start_ = 0;

  std::vector<TradeRecord> trade_history_;

  double stop_loss_pct;
  double take_profit_pct;
  double taker_fee_pct = 0.0004;
  double slippage = 5.0;         
  double total_fees_paid = 0.0;  

  // Hilfsfunktion: Setzt den Tages-PnL um 00:00 UTC auf 0 zurück
  void UpdateDay(int64_t current_time) {
    int64_t day_ms = 86400000; // 24 Stunden in Millisekunden
    int64_t new_day = current_time - (current_time % day_ms);
    
    if (current_day_start_ == 0) {
      current_day_start_ = new_day;
    } else if (new_day > current_day_start_) {
      current_day_start_ = new_day;
      current_daily_pnl_ = 0.0; 
    }
  }

  void ClosePosition(double current_price, const std::string &reason, int64_t current_time) {
    if (position_size > 0.0) {
      double actual_exit_price;
      double gross_profit;

      if (position_direction_ == SignalDirection::BUY) {
        actual_exit_price = current_price - slippage;
        gross_profit = (actual_exit_price - entry_price) * position_size;
      } else {
        actual_exit_price = current_price + slippage;
        gross_profit = (entry_price - actual_exit_price) * position_size;
      }

      double exit_fee = (actual_exit_price * position_size) * taker_fee_pct;
      total_fees_paid += exit_fee;
      double net_profit = gross_profit - exit_fee;
      
      balance += net_profit;
      
      // Das finale Trade-Ergebnis wird auf den heutigen Tag gebucht
      current_daily_pnl_ += net_profit; 

      if (net_profit > 0) trades_won++;
      else trades_lost++;

      trade_history_.push_back({entry_time_, current_time, position_direction_, entry_price, actual_exit_price, net_profit, reason});

      position_size = 0.0;
      entry_price = 0.0;
      current_sl_ = 0.0;
      current_tp_ = 0.0;
      entry_time_ = 0;
      sl_moved_to_be_ = false;
      position_direction_ = SignalDirection::NONE;
    }
  }
};