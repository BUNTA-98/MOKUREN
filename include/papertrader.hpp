#pragma once
#include "aggregator.hpp"
#include <iostream>
#include <string>
#include <vector>

class PaperTrader {

public:

  PaperTrader(double sl_pct = 0.005, double tp_pct = 0.01)
      : stop_loss_pct(sl_pct), take_profit_pct(tp_pct) {}


  double GetBalance() const { return balance; }
  int GetTradesWon() const { return trades_won; }
  int GetTradesLost() const { return trades_lost; }
  double GetTotalFeesPaid() const { return total_fees_paid; }
  int GetTotalTrades() const { return trades_won + trades_lost; }
  double GetPositionSize() const { return position_size; }
  void UpdateStopLoss(double new_sl) {current_sl_ = new_sl; }
  bool HasOpenPosition() const {return position_size > 0;}
  const std::vector<std::string> &GetTradeLog() const { return trade_log_; }
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
    double winrate = 0.0;
    if (total_trades > 0) {
      winrate = (static_cast<double>(trades_won) / total_trades) * 100.0;
    }
    return winrate;
  }


  void CheckRisk(double current_price) {
    if (position_size == 0.0)
      return;

    if (position_direction_ == SignalDirection::BUY) {
      // long: sl ist unten, tp ist oben
      if (current_price <= current_sl_)
        ClosePosition(current_price, "sl");
      else if (current_price >= current_tp_)
        ClosePosition(current_price, "tp");
    } else if (position_direction_ == SignalDirection::SELL) {
      // short: sl ist oben, tp ist unten
      if (current_price >= current_sl_)
        ClosePosition(current_price, "sl");
      else if (current_price <= current_tp_)
        ClosePosition(current_price, "tp");
    }
  }

  void ProcessSignal(TradeSignal signal, double current_price) {
    // schutz vor doppel-ausführung
    if (position_size > 0.0)
      return;

    if (signal.volume > 0.0 && (signal.direction == SignalDirection::BUY ||
                                signal.direction == SignalDirection::SELL)) {
      position_size = signal.volume;
      position_direction_ = signal.direction;
      current_sl_ = signal.stop_loss;
      current_tp_ = signal.take_profit;

      // slippage berechnen
      if (position_direction_ == SignalDirection::BUY) {
        entry_price = current_price + slippage; // teurer kaufen
      } else {
        entry_price = current_price - slippage; // billiger shorten
      }

      double entry_fee = (entry_price * position_size) * taker_fee_pct;
      balance -= entry_fee;
      total_fees_paid += entry_fee;
    }
  }

  void CloseOpenPositionAtEnd(double final_price) {
    ClosePosition(final_price, "end of backtest");
  }

  void PrintResults() const {
    std::cout << "\n=== $$$ backtest ergebnis $$$ ===\n"
              << "startkapital       : 10000.00 usdt\n"
              << "endkapital         : " << balance << " usdt\n"
              << "netto profit       : " << (balance - 10000.0) << " usdt\n"
              << "gezahlte gebuehren : " << total_fees_paid << " usdt\n"
              << "gewonnene trades   : " << trades_won << "\n"
              << "verlorene trades   : " << trades_lost << "\n"
              << "===============================\n";
  }
  



private:
  double balance = 10000.0;
  double position_size = 0.0;
  double entry_price = 0.0;
  int trades_won = 0;
  int trades_lost = 0;

  // neu: trackt ticket limits und richtung
  SignalDirection position_direction_ = SignalDirection::NONE;
  double current_sl_ = 0.0;
  double current_tp_ = 0.0;

  std::vector<std::string> trade_log_;

  // alte pct-werte behalten wir für fallback oder kompatibilität
  double stop_loss_pct;
  double take_profit_pct;


  double taker_fee_pct = 0.0004; // 0.04% pro ausführung
  double slippage = 5.0;         // 1$ preisrutsch
  double total_fees_paid = 0.0;  // gebühren tracker

  void ClosePosition(double current_price, const std::string &reason) {
    if (position_size > 0.0) {
      double actual_exit_price;
      double gross_profit;

      // richtungsabhängige logik
      if (position_direction_ == SignalDirection::BUY) {
        actual_exit_price = current_price - slippage; // tiefer verkaufen
        gross_profit = (actual_exit_price - entry_price) * position_size;
      } else {
        actual_exit_price = current_price + slippage; // höher zurückkaufen
        gross_profit = (entry_price - actual_exit_price) * position_size;
      }

      double exit_fee = (actual_exit_price * position_size) * taker_fee_pct;
      total_fees_paid += exit_fee;

      double net_profit = gross_profit - exit_fee;
      balance += net_profit;

      if (net_profit > 0)
        trades_won++;
      else
        trades_lost++;

      // reset
      position_size = 0.0;
      entry_price = 0.0;
      current_sl_ = 0.0;
      current_tp_ = 0.0;
      position_direction_ = SignalDirection::NONE;
    }
  }
};
