#include "papertrader.hpp"
#include "logger.hpp"
#include <cmath>

PaperTrader::PaperTrader(double slip, double taker_fee,
                         double initial_balance)
    : slippage(slip), taker_fee_pct(taker_fee) {
  // a shadow run (--paper) seeds the account from cfg.initial_balance so its
  // drawdown is measured against the capital it will actually be funded with.
  // a non-positive seed is not a valid account: UpdateDrawdown divides by
  // peak_balance_, so 0.0 turned every drawdown into NaN instead of an error.
  // AppConfig::Validate rejects it at the config boundary; this guard covers
  // direct construction (tests, tooling).
  if (!(initial_balance > 0.0) || !std::isfinite(initial_balance)) {
    LOG_WARN("PaperTrader: invalid initial_balance={}; falling back to {}",
             initial_balance, kInitialBalance);
    initial_balance = kInitialBalance;
  }

  // the old first argument was an absolute dollar amount, so a caller that
  // passed only that number now binds it to slippage -- 400.0 would slip every
  // fill by 400x. AppConfig::Validate already bounds both of these (>= 0.0
  // and < 0.1), so an out-of-range value here is a stale call site rather
  // than a strategy decision.
  if (!(slippage >= 0.0) || slippage >= 0.1) {
    LOG_WARN("PaperTrader: invalid slippage={}; falling back to 0.0002",
             slippage);
    slippage = 0.0002;
  }
  if (!(taker_fee_pct >= 0.0) || taker_fee_pct >= 0.1) {
    LOG_WARN("PaperTrader: invalid taker_fee={}; falling back to 0.0004",
             taker_fee_pct);
    taker_fee_pct = 0.0004;
  }

  initial_balance_ = initial_balance;
  balance = initial_balance;
  peak_balance_ = initial_balance;
}

TradeSignal PaperTrader::GetCurrentPosition() const {
  TradeSignal sig;
  sig.direction = position_direction_;
  sig.volume = position_size;
  sig.entry_price = entry_price;
  sig.stop_loss = current_sl_;
  sig.take_profit = current_tp_;
  return sig;
}

double PaperTrader::FloatingNetProfit(double price) const {
  if (position_size <= 0.0 || position_direction_ == SignalDirection::NONE) return 0.0;
  // mirror the exit maths in CheckRisk/ClosePosition: an exit crosses the
  // spread, so the mark is slipped against the position before the fee.
  const bool long_side = (position_direction_ == SignalDirection::BUY);
  const double exit_price = long_side ? price * (1.0 - slippage) : price * (1.0 + slippage);
  const double gross = long_side ? (exit_price - entry_price) * position_size
                                 : (entry_price - exit_price) * position_size;
  return gross - exit_price * position_size * taker_fee_pct;
}

double PaperTrader::GetEquityAt(double mark) const {
  // flat -> nothing floats. no mark -> cash, not a fabricated loss.
  if (position_size <= 0.0 || !(mark > 0.0)) return balance;
  return balance + FloatingNetProfit(mark);
}

double PaperTrader::GetEquity() const {
  // the account's own view: whatever price it was last ticked with.
  return GetEquityAt(last_mark_price_);
}

double PaperTrader::GetEquityAtBidAsk(double bid, double ask) const {
  // flat -> nothing floats.
  if (position_size <= 0.0 || position_direction_ == SignalDirection::NONE) {
    return balance;
  }
  // a long exits at the bid, a short at the ask. this is the whole point of
  // the method: GetEquityAt(mark) treats its argument as a mid and slips it
  // direction-aware, which is wrong for a one-sided quote.
  const bool long_side = (position_direction_ == SignalDirection::BUY);
  const double exit_price = long_side ? bid : ask;
  // no usable quote -> cash, not a fabricated loss (mirrors GetEquityAt).
  if (!(exit_price > 0.0)) return balance;
  const double gross = long_side
      ? (exit_price - entry_price) * position_size
      : (entry_price - exit_price) * position_size;
  return balance + gross - exit_price * position_size * taker_fee_pct;
}

double PaperTrader::GetWinrate() const {
  double total_trades = GetTotalTrades();
  if (total_trades > 0) return (static_cast<double>(trades_won) / total_trades) * 100.0;
  return 0.0;
}

void PaperTrader::ApplyManagementConfig(double be_trig, double be_targ, 
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

void PaperTrader::CheckRisk(double current_price, int64_t current_time) {
  last_mark_price_ = current_price;
  if (position_size == 0.0) return;

  // track price extremes for trail
  if (current_price > highest_seen_price_) highest_seen_price_ = current_price;
  if (current_price < lowest_seen_price_ || lowest_seen_price_ == 0.0) lowest_seen_price_ = current_price;

  // calc float equity -- the same maths GetEquity() uses to mark the account,
  // kept in one place so the guard and the internal stop cannot drift apart.
  const double floating_net_profit = FloatingNetProfit(current_price);

  UpdateDrawdown(balance + floating_net_profit);

  // NOTE: no daily-loss stop here. the account's daily limit belongs to
  // PropGuard (prop_equity.hpp on the backtest paths, LiveOrchestrator on the
  // venue path): one anchor (day-start equity), one unit (fraction of
  // initial_balance) and one clock (prop.daily_reset_hour_utc). a stop in this
  // object would be a second ladder that disagrees with it -- and on the venue
  // path it never ran at all, since BybitBroker::CheckRisk is empty.

  // eval partial scale out
  if (enable_scale_out_ && !has_scaled_out_) {
      bool hit_scale_long = (position_direction_ == SignalDirection::BUY && current_price >= entry_price * (1.0 + scale_out_trigger_pct_));
      bool hit_scale_short = (position_direction_ == SignalDirection::SELL && current_price <= entry_price * (1.0 - scale_out_trigger_pct_));
      
      if (hit_scale_long || hit_scale_short) {
          LOG_INFO("scale-out trigger: dir={} price={} entry={} fraction={} t={}",
                   static_cast<int>(position_direction_), current_price,
                   entry_price, scale_out_fraction_, current_time);
          ClosePartial(scale_out_fraction_, current_price, current_time, "scale_out");
          has_scaled_out_ = true;
          current_sl_ = (position_direction_ == SignalDirection::BUY) ? entry_price * (1.0 + be_target_pct_) : entry_price * (1.0 - be_target_pct_);
          sl_moved_to_be_ = true;
          current_stop_history_.push_back({current_time, 0, current_sl_, "SCALE->BE"});
      }
  }

  // eval long trade mgmt
  if (position_direction_ == SignalDirection::BUY) {
    if (!sl_moved_to_be_ && current_price >= entry_price * (1.0 + be_trigger_pct_)) {
      current_sl_ = entry_price * (1.0 + be_target_pct_);
      sl_moved_to_be_ = true;
      current_stop_history_.push_back({current_time, 0, current_sl_, "BE TRIGGER"});
    }
    
    if (enable_trailing_ && current_price >= entry_price * (1.0 + trailing_trigger_pct_)) {
        double proposed_sl = highest_seen_price_ * (1.0 - trailing_dist_pct_);
        if (proposed_sl > current_sl_) {
            current_sl_ = proposed_sl; 
            current_stop_history_.push_back({current_time, 0, current_sl_, "TRAIL MOVE"});
        }
    }

    if (current_price <= current_sl_) ClosePosition(current_price, sl_moved_to_be_ ? "trail/be" : "sl", current_time);
    else if (current_price >= current_tp_) ClosePosition(current_price, "tp", current_time);
      
  // eval short trade mgmt
  } else if (position_direction_ == SignalDirection::SELL) {
    if (!sl_moved_to_be_ && current_price <= entry_price * (1.0 - be_trigger_pct_)) {
      current_sl_ = entry_price * (1.0 - be_target_pct_);
      sl_moved_to_be_ = true;
      current_stop_history_.push_back({current_time, 0, current_sl_, "BE TRIGGER"});
    }

    if (enable_trailing_ && current_price <= entry_price * (1.0 - trailing_trigger_pct_)) {
        double proposed_sl = lowest_seen_price_ * (1.0 + trailing_dist_pct_);
        if (proposed_sl < current_sl_ || current_sl_ == 0.0) {
            current_sl_ = proposed_sl; 
            current_stop_history_.push_back({current_time, 0, current_sl_, "TRAIL MOVE"});
        }
    }

    if (current_price >= current_sl_) ClosePosition(current_price, sl_moved_to_be_ ? "trail/be" : "sl", current_time);
    else if (current_price <= current_tp_) ClosePosition(current_price, "tp", current_time);
  }
}

void PaperTrader::ProcessSignal(TradeSignal signal, double fill_price, int64_t current_time) {
  // mark before the entry, so GetEquity() is meaningful the moment the guard
  // reads it instead of waiting for the next CheckRisk tick.
  if (fill_price > 0.0) last_mark_price_ = fill_price;

  if (position_size > 0.0) return;

  if (signal.volume > 0.0 && (signal.direction == SignalDirection::BUY ||
                              signal.direction == SignalDirection::SELL)) {
    // the entry maths below divides by entry_price and re-derives sl/tp from
    // percentage distances. a zero entry makes those NaN, and a zero or
    // inverted stop leaves the position without a stop for the rest of the
    // trade -- both fail silently as "never exits".
    const bool long_side = (signal.direction == SignalDirection::BUY);
    const bool geometry_ok =
        signal.entry_price > 0.0 &&
        (long_side ? (signal.stop_loss > 0.0 &&
                      signal.stop_loss < signal.entry_price &&
                      signal.take_profit > signal.entry_price)
                   : (signal.stop_loss > signal.entry_price &&
                      signal.take_profit > 0.0 &&
                      signal.take_profit < signal.entry_price));
    if (!geometry_ok) {
      LOG_ERROR("entry rejected: invalid geometry dir={} entry={} sl={} tp={} vol={}",
                static_cast<int>(signal.direction), signal.entry_price,
                signal.stop_loss, signal.take_profit, signal.volume);
      return;
    }

    position_size = signal.volume;
    position_direction_ = signal.direction;
    entry_time_ = current_time;
    
    sl_moved_to_be_ = false;
    has_scaled_out_ = false;
    highest_seen_price_ = fill_price;
    lowest_seen_price_ = fill_price;
    current_stop_history_.clear(); 

    // apply simulated slippage
    if (position_direction_ == SignalDirection::BUY) {
      entry_price = fill_price * (1.0 + slippage);
      
      // shift sl/tp relative to slipped entry
      double sl_dist_pct = (signal.entry_price - signal.stop_loss) / signal.entry_price;
      double tp_dist_pct = (signal.take_profit - signal.entry_price) / signal.entry_price;
      current_sl_ = entry_price * (1.0 - sl_dist_pct);
      current_tp_ = entry_price * (1.0 + tp_dist_pct);
    } else {
      entry_price = fill_price * (1.0 - slippage);

      double sl_dist_pct = (signal.stop_loss - signal.entry_price) / signal.entry_price;
      double tp_dist_pct = (signal.entry_price - signal.take_profit) / signal.entry_price;
      current_sl_ = entry_price * (1.0 + sl_dist_pct);
      current_tp_ = entry_price * (1.0 - tp_dist_pct);
    }

    // deduct fees
    double entry_fee = (entry_price * position_size) * taker_fee_pct;
    current_entry_fee_ = entry_fee; 
    balance -= entry_fee;
    total_fees_paid += entry_fee;
    
    UpdateDrawdown(balance);

    LOG_INFO("entry: dir={} fill={} entry={} sl={} tp={} vol={} slip={} fee={} balance={} t={}",
             static_cast<int>(position_direction_), fill_price, entry_price,
             current_sl_, current_tp_, position_size, slippage,
             entry_fee, balance, current_time);
  }
}

void PaperTrader::CloseOpenPositionAtEnd(double final_price, int64_t end_time) {
  ClosePosition(final_price, "end of backtest", end_time);
}

void PaperTrader::UpdateDrawdown(double current_equity) {
  if (current_equity > peak_balance_) peak_balance_ = current_equity;
  double current_dd = (peak_balance_ - current_equity) / peak_balance_ * 100.0;
  if (current_dd > max_drawdown_pct_) max_drawdown_pct_ = current_dd;
}

  

void PaperTrader::ClosePartial(double fraction, double current_price, int64_t current_time, const std::string &reason) {
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
    UpdateDrawdown(balance);

    double partial_entry_fee = current_entry_fee_ * fraction;
    current_entry_fee_ -= partial_entry_fee;
    double true_trade_pnl = net_profit - partial_entry_fee;

    LOG_INFO("partial exit: reason={} dir={} price={} exit={} vol={} slip={} fee={} pnl={} balance={} t={}",
             reason, static_cast<int>(position_direction_), current_price,
             actual_exit_price, close_volume, slippage, exit_fee,
             true_trade_pnl, balance, current_time);

    // save log record
    TradeRecord rec;
    rec.entry_time = entry_time_;
    rec.exit_time = current_time;
    rec.direction = position_direction_;
    rec.entry_price = entry_price;
    rec.exit_price = actual_exit_price;
    rec.net_profit = true_trade_pnl;
    rec.exit_reason = reason;
    rec.stop_events = current_stop_history_;

    trade_history_.push_back(rec);
    // the live broadcaster reads this after any exit, not just a full close.
    last_closed_size_ = close_volume;
    position_size -= close_volume; 

    // a full reduction through this path -- the prop guard's flatten, or an
    // explicit ClosePartial(1.0) -- must leave the same clean state as
    // ClosePosition. otherwise position_direction_ and entry_price survive a
    // closed position and GetCurrentPosition reports one that does not exist.
    if (position_size <= 0.0) {
      position_size = 0.0;

      // a full close via the partial path IS a completed trade: book it in the
      // same win/loss/be ledger ClosePosition uses, or GetTotalTrades() and
      // GetWinrate() silently drop every prop_guard flatten. the scan CSV
      // reads those two, and the trade_history_ already contains the record,
      // so leaving the counters alone reported "Trades=0, SL%=100" on a
      // breached permutation that was flattened out of a single losing trade.
      //
      // scale-outs (fraction < 1.0) leave position_size > 0.0 and are still
      // not counted here; that matches ClosePosition, which only books a trade
      // when the position is finally emptied.
      if (net_profit > 0.0) trades_won++;
      else if (net_profit < 0.0) trades_lost++;
      else trades_be++;
      entry_price = 0.0;
      current_sl_ = 0.0;
      current_tp_ = 0.0;
      entry_time_ = 0;
      current_entry_fee_ = 0.0;
      sl_moved_to_be_ = false;
      has_scaled_out_ = false;
      position_direction_ = SignalDirection::NONE;
      current_stop_history_.clear();
    }
}

void PaperTrader::ClosePosition(double current_price, const std::string &reason, int64_t current_time) {
  if (position_size > 0.0) {
    double actual_exit_price, gross_profit;

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
    UpdateDrawdown(balance);

    // remember the closed volume for the live event broadcaster.
    last_closed_size_ = position_size;

    if (net_profit > 0) trades_won++;
    else if (net_profit < 0) trades_lost++;
    else trades_be++;

    double true_trade_pnl = net_profit - current_entry_fee_;

    LOG_INFO("exit: reason={} dir={} price={} exit={} vol={} slip={} fee={} pnl={} balance={} t={}",
             reason, static_cast<int>(position_direction_), current_price,
             actual_exit_price, position_size, slippage, exit_fee,
             true_trade_pnl, balance, current_time);

    // save final log record
    TradeRecord rec;
    rec.entry_time = entry_time_;
    rec.exit_time = current_time;
    rec.direction = position_direction_;
    rec.entry_price = entry_price;
    rec.exit_price = actual_exit_price;
    rec.net_profit = true_trade_pnl;
    rec.exit_reason = reason;
    rec.stop_events = current_stop_history_;

    trade_history_.push_back(rec);

    // reset internal state for next trade
    position_size = 0.0;
    entry_price = 0.0;
    current_sl_ = 0.0;
    current_tp_ = 0.0;
    entry_time_ = 0;
    current_entry_fee_ = 0.0; 
    sl_moved_to_be_ = false;
    has_scaled_out_ = false;
    position_direction_ = SignalDirection::NONE;
    current_stop_history_.clear();
  }
}
