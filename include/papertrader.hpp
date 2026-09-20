#pragma once
#include "aggregator.hpp"
#include <iostream>
#include <string>

class PaperTrader {
private:
    double balance = 10000.0;
    double position_size = 0.0;
    double entry_price = 0.0;
    int trades_won = 0;
    int trades_lost = 0;
    
    // Risikomanagement Parameter
    double stop_loss_pct;
    double take_profit_pct;

    // DRY (Don't Repeat Yourself): Zentrale Methode zum Schließen der Position
    void ClosePosition(double current_price, const std::string& reason) {
        if (position_size > 0.0) {
            double profit = (current_price - entry_price) * position_size;
            balance += profit;
            
            if (profit > 0) {
                trades_won++;
                std::cout << "[TRADER] Position GESCHLOSSEN (" << reason << " - GEWINN): +" << profit << " USDT @ " << current_price << std::endl;
            } else {
                trades_lost++;
                std::cout << "[TRADER] Position GESCHLOSSEN (" << reason << " - VERLUST): " << profit << " USDT @ " << current_price << std::endl;
            }
            
            // Position sauber zurücksetzen
            position_size = 0.0;
            entry_price = 0.0;
        }
    }

public:
    // Konstruktor mit Standardwerten (0.5% SL, 1.0% TP), kann aus der Main überschrieben werden
    PaperTrader(double sl_pct = 0.005, double tp_pct = 0.01) 
        : stop_loss_pct(sl_pct), take_profit_pct(tp_pct) {}

    // Wird BEI JEDEM EINZELNEN TICK in der main aufgerufen
    void CheckRisk(double current_price) {
        if (position_size == 0.0) return; // Keine Position offen -> Nichts zu tun

        // Prozentuale Veränderung berechnen (nur für Long-Positionen)
        double price_change_pct = (current_price - entry_price) / entry_price;

        if (price_change_pct <= -stop_loss_pct) {
            ClosePosition(current_price, "STOP LOSS");
        } 
        else if (price_change_pct >= take_profit_pct) {
            ClosePosition(current_price, "TAKE PROFIT");
        }
    }

    void ProcessSignal(eSignal signal, double current_price) {
        if (signal == eSignal::BUY) {
            // Nur kaufen, wenn wir noch nicht investiert sind
            if (position_size == 0.0) {
                position_size = 0.1; 
                entry_price = current_price;
                std::cout << "[TRADER] Position GEÖFFNET (BUY) zu " << entry_price << std::endl;
            }
        }
        else if (signal == eSignal::SELL) {
            ClosePosition(current_price, "SELL SIGNAL");
        }
    }

    void CloseOpenPositionAtEnd(double final_price) {
        ClosePosition(final_price, "END OF BACKTEST");
    }

    void PrintResults() const {
        std::cout << "\n=== $$$ BACKTEST ERGEBNIS $$$ ===\n"
                  << "Startkapital     : 10000.00 USDT\n"
                  << "Endkapital       : " << balance << " USDT\n"
                  << "Netto Profit     : " << (balance - 10000.0) << " USDT\n"
                  << "Gewonnene Trades : " << trades_won << "\n"
                  << "Verlorene Trades : " << trades_lost << "\n"
                  << "===============================\n";
    }
};
