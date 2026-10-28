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
  // NOTE: no daily-loss argument. an account's daily limit is policy, not
  // account state: PropGuard measures it against day-start EQUITY, resets it
  // on prop.daily_reset_hour_utc and enforces it on every path. the ladder
  // this constructor used to take was a second daily rule anchored on today's
  // realised pnl, denominated in absolute dollars (so it did not scale with
  // cfg.initial_balance) and reset at a hardcoded utc midnight -- and because
  // BybitBroker::CheckRisk is empty, it never ran on the venue path at all.
  PaperTrader(double slip = 0.0002,
              double taker_fee = 0.0004,
              double initial_balance = kInitialBalance);

  // basic state getters
  double GetBalance() const override { return balance; }
  // mark-to-market equity, the number the firm measures: realised cash plus
  // the floating pnl of the open position, net of the exit fee it would pay.
  // the mark is the last price handed to CheckRisk/ProcessSignal; with no
  // position (or no price yet) equity is just the balance. kept separate from
  // GetBalance() because the prop guard must see an open drawdown the instant
  // it happens, not when the position finally closes.
  double GetEquity() const override;
  // mark at `mark` instead of at the last price this account was ticked with.
  // the backtest loops hold the current price and hand it to the guard on the
  // same print, so deriving the floating leg there from last_mark_price_ would
  // measure equity one tick behind the bar being evaluated. identical maths to
  // GetEquity() otherwise -- one floating leg, net of the exit it would pay.
  //
  // NOTE: `mark` is treated as a mid. FloatingNetProfit applies direction-
  // aware slippage to it, which is correct for a mid or a trade print but
  // WRONG for a one-sided quote: passing best_bid for a short exits at
  // best_bid * (1 + slip), i.e. on the wrong side of the book. callers that
  // hold both sides of the book must use GetEquityAtBidAsk instead.
  double GetEquityAt(double mark) const override;
  // mark-to-market equity with the executable close price given by side: a
  // long closes at the bid, a short closes at the ask. bid/ask already IS the
  // spread, so no further slippage is applied -- this is what a market-order
  // flatten would actually receive at that instant, which is the accounting
  // the prop guard is measuring. flat or no usable quote -> cash.
  double GetEquityAtBidAsk(double bid, double ask) const override;
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
  // gross minus the exit fee closing at `price` would pay. balance already has
  // the entry fee taken out, so balance + this is equity without charging the
  // entry twice.
  double FloatingNetProfit(double price) const;
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
  // last price seen, used by GetEquity() to mark the open position. zero until
  // the first CheckRisk/ProcessSignal, which is why GetEquity() falls back to
  // the balance rather than marking against 0 and fabricating a total loss.
  double last_mark_price_ = 0.0;

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
