#pragma once
#include "aggregator.hpp"
#include "ibroker.hpp"
#include "logger.hpp"
#include <limits>
#include <string>
#include <vector>

// simulated broker for backtesting and execution logic
class PaperTrader : public IBroker {
public:
  // starting account size. one constant instead of the literal 10000 that was
  // copied into the constructor, GetNetProfit and the WFA summary.
  static constexpr double kInitialBalance = 10000.0;

  // initial_balance seeds the account. --paper (shadow) runs pass
  // cfg.initial_balance so drawdown is measured against the capital the
  // account will actually be funded with; every other path keeps the
  // compile-time default and therefore behaves exactly as before.
  PaperTrader(double max_dl = 400.0, double slip = 0.0002,
              double taker_fee = 0.0004,
              double initial_balance = kInitialBalance);

  // basic state getters
  double GetBalance() const override { return balance; }
  bool HasOpenPosition() const override { return position_size > 0; }
  double GetPositionSize() const { return position_size; }
  void UpdateStopLoss(double new_sl) override { current_sl_ = new_sl; }
  TradeSignal GetCurrentPosition() const override;

  // interface implementation for partial closes
  void ClosePartial(double fraction, double current_price, int64_t current_time, const std::string& reason) override;

  // performance metric getters
  int GetTradesWon() const { return trades_won; }
  int GetTradesLost() const { return trades_lost; }
  int GetTradesBE() const { return trades_be; } 
  int GetTotalTrades() const { return trades_won + trades_lost + trades_be; }
  double GetTotalFeesPaid() const { return total_fees_paid; }
  // net of the balance this account actually started with. subtracting the
  // compile-time constant reported a seeded account (--paper with
  // initial_balance=50000) as +40000 on its first tick, and would have made a
  // losing shadow run look profitable.
  double GetNetProfit() const { return balance - initial_balance_; }
  double GetMaxDrawdown() const { return max_drawdown_pct_; }
  std::vector<TradeRecord> GetTradeHistory() const override { return trade_history_; }
  // single-threaded paper account: no lock needed, unlike the ws-backed brokers.
  int64_t GetLastTradeTime() const override {
    return trade_history_.empty() ? 0 : trade_history_.back().exit_time;
  }
  size_t GetTradeCount() const override { return trade_history_.size(); }
  double GetLastClosedSize() const override { return last_closed_size_; }
  double GetWinrate() const;

  // inject active trade management config
  void ApplyManagementConfig(double be_trig, double be_targ, 
                             bool trail_en, double trail_trig, double trail_dist,
                             bool scale_en, double scale_trig, double scale_frac);

  // Keep executing the stop and the target, but stop managing the trade. The
  // r-multiple ladder (break-even, trailing, scale-out) belongs to
  // PositionManager, which measures every threshold against the placed stop.
  // Running both moved the same stop twice and could scale out twice.
  void DisableManagement() {
    be_trigger_pct_ = std::numeric_limits<double>::infinity();
    enable_trailing_ = false;
    enable_scale_out_ = false;
  }

  // core engine hooks
  void CheckRisk(double current_price, int64_t current_time) override;
  void ProcessSignal(TradeSignal signal, double fill_price, int64_t current_time) override;
  void CloseOpenPositionAtEnd(double final_price, int64_t end_time);

private:
  // internal helpers
  void UpdateDrawdown(double current_equity);
  void UpdateDay(int64_t current_time);
  void ClosePosition(double current_price, const std::string &reason, int64_t current_time);

  // account state. initialised in-class so no path can read an uninitialised
  // account (the WFA aggregation and PositionSizer::GetBalance read it early).
  double balance = kInitialBalance;
  double peak_balance_ = kInitialBalance;
  // the seed this account started from. kept so GetNetProfit and the drawdown
  // math measure against the funded capital rather than the 10k default.
  double initial_balance_ = kInitialBalance;
  double max_drawdown_pct_ = 0.0;
  
  // position state
  double position_size = 0.0;
  double entry_price = 0.0;
  SignalDirection position_direction_ = SignalDirection::NONE;
  double current_sl_ = 0.0;
  double current_tp_ = 0.0;
  int64_t entry_time_ = 0;
  double current_entry_fee_ = 0.0; 
  double last_closed_size_ = 0.0;

  // trade management state
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

  // risk state
  double max_daily_loss_;
  double current_daily_pnl_ = 0.0;
  int64_t current_day_start_ = 0;

  // metrics and logs
  int trades_won = 0;
  int trades_lost = 0;
  int trades_be = 0; 
  std::vector<TradeRecord> trade_history_;
  std::vector<StopEvent> current_stop_history_; 

  // static config constraints
  double slippage;         
  double taker_fee_pct;
  double total_fees_paid = 0.0;  
};
