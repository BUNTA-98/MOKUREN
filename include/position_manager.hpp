#pragma once
#include "papertrader.hpp"
#include <cmath>

class PositionManager {
private:
  PaperTrader* broker;
  bool stop_moved_to_be = false;

public:
  PositionManager(PaperTrader* p) : broker(p) {}

  void Update(double current_price) {
    // Wenn kein Trade offen ist, Reset-Flag für den nächsten Trade setzen
    if (!broker->HasOpenPosition()) {
      stop_moved_to_be = false; 
      return;
    }

    // Wenn der Stop-Loss in diesem Trade bereits nachgezogen wurde, nichts tun
    if (stop_moved_to_be) return; 

    TradeSignal pos = broker->GetCurrentPosition();
    
    // Die halbe Strecke zum Ziel berechnen
    double distance_to_tp = std::abs(pos.take_profit - pos.entry_price);
    double half_way = distance_to_tp * 0.5;

    // Dynamisches Stop-Loss-Trailing (Break-Even Logik)
    if (pos.direction == SignalDirection::BUY) {
      if (current_price >= pos.entry_price + half_way) {
        broker->UpdateStopLoss(pos.entry_price);
        stop_moved_to_be = true;
      }
    } else if (pos.direction == SignalDirection::SELL) {
      if (current_price <= pos.entry_price - half_way) {
        broker->UpdateStopLoss(pos.entry_price);
        stop_moved_to_be = true;
      }
    }
  }
};