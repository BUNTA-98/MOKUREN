#pragma once

struct EngineConfig {
  // Zeitintervall für Kerzen in MILLISEKUNDEN:
  //  1 Sekunde = 1000
  //  1 Minute  = 60000      (60 * 1000)
  //  5 Minuten = 300000     (5 * 60 * 1000)
  int64_t interval_ms = 60000; // 1-Minuten-Kerzen (Propfirm-konform)
  double tick_size = 1.0;
  double imbalance_threshold = 2.5; // Schwellenwert für Orderflow-Imbalance
  double stop_loss_pct = 0.005;     // 0.5% Stop Loss
  double take_profit_pct = 0.01;    // 1.0% Take Profit
};
