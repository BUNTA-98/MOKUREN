// TODO
// trader should calc winrate usw himself

#pragma once
#include "aggregator.hpp"
#include <iostream>
#include <vector>
#include <string>

class PaperTrader {
private:
  double balance = 10000.0;
  double position_size = 0.0;
  double entry_price = 0.0;
  int trades_won = 0;
  int trades_lost = 0;
  
  std::vector<std::string> trade_log_;


  // Risikomanagement & Realismus
  double stop_loss_pct;
  double take_profit_pct;

  double taker_fee_pct = 0.0004; // 0.04% pro Ausführung
  double slippage = 1.0;         // 1$ Preisrutsch (angenommen 1 Tick)
  double total_fees_paid = 0.0;  // Trackt, wie viel wir an die Börse zahlen

  void ClosePosition(double current_price, const std::string &reason) {
    if (position_size > 0.0) {
      // Slippage: Beim Verkaufen bekommen wir einen leicht schlechteren Preis
      double actual_exit_price = current_price - slippage;

      // Brutto-Profit berechnen
      double gross_profit = (actual_exit_price - entry_price) * position_size;

      // Gebühr für das Schließen berechnen und abziehen
      double exit_fee = (actual_exit_price * position_size) * taker_fee_pct;
      total_fees_paid += exit_fee;

      // Netto-Profit auf die Balance schlagen
      double net_profit = gross_profit - exit_fee;
      balance += net_profit;

      if (net_profit > 0) {
        trades_won++;
        
        /*
        std::cout << "[TRADER] Position GESCHLOSSEN (" << reason
                  << " - GEWINN): +" << net_profit << " USDT @ "
                  << actual_exit_price << std::endl;
        */
      } else {
        trades_lost++;
        /*
        std::cout << "[TRADER] Position GESCHLOSSEN (" << reason
                  << " - VERLUST): " << net_profit << " USDT @ "
                  << actual_exit_price << std::endl;
        */
      }

      position_size = 0.0;
      entry_price = 0.0;
    }
  }

public:
  PaperTrader(double sl_pct = 0.005, double tp_pct = 0.01)
      : stop_loss_pct(sl_pct), take_profit_pct(tp_pct) {}

  // getter für ui
  double GetBalance() const { return balance; }

  int GetTradesWon() const { return trades_won; }

  int GetTradesLost() const { return trades_lost; }

  double GetTotalFeesPaid() const { return total_fees_paid; }

  int GetTotalTrades() const { return trades_won + trades_lost; }

  const std::vector<std::string>& GetTradeLog() const { return trade_log_; }

  double GetNetProfit() const {
    return balance - 10000.0;
  } // 10000.0 ist das Startkapital

  double GetWinrate() const {
    double total_trades = GetTotalTrades();
    double winrate = 0.0;
    if (total_trades > 0) {
      winrate = (static_cast<double>(trades_won / total_trades) * 100.0);
    }
    return winrate;
  }

  void CheckRisk(double current_price) {
    if (position_size == 0.0)
      return;

    double price_change_pct = (current_price - entry_price) / entry_price;

    if (price_change_pct <= -stop_loss_pct) {
      ClosePosition(current_price, "STOP LOSS");
    } else if (price_change_pct >= take_profit_pct) {
      ClosePosition(current_price, "TAKE PROFIT");
    }
  }

  void ProcessSignal(eSignal signal, double current_price) {
    if (signal == eSignal::BUY) {
      if (position_size == 0.0) {
        position_size = 0.1;

        // Slippage: Beim Kaufen zahlen wir minimal mehr
        entry_price = current_price + slippage;

        // Gebühr für das Öffnen berechnen und direkt abziehen
        double entry_fee = (entry_price * position_size) * taker_fee_pct;
        balance -= entry_fee;
        total_fees_paid += entry_fee;

        /*std::cout << "[TRADER] Position GEÖFFNET (BUY) zu " << entry_price
                  << " | Fee: " << entry_fee << std::endl;
        */
      }
    } else if (signal == eSignal::SELL) {
      ClosePosition(current_price, "SELL SIGNAL");
    }
  }

  void CloseOpenPositionAtEnd(double final_price) {
    ClosePosition(final_price, "END OF BACKTEST");
  }

  void PrintResults() const {
    std::cout << "\n=== $$$ BACKTEST ERGEBNIS $$$ ===\n"
              << "Startkapital       : 10000.00 USDT\n"
              << "Endkapital         : " << balance << " USDT\n"
              << "Netto Profit       : " << (balance - 10000.0) << " USDT\n"
              << "Gezahlte Gebuehren : " << total_fees_paid << " USDT\n"
              << "Gewonnene Trades   : " << trades_won << "\n"
              << "Verlorene Trades   : " << trades_lost << "\n"
              << "===============================\n";
  }
};
