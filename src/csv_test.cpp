#include "aggregator.hpp"
#include "csv_parser.hpp"
#include "engine_config.hpp"
#include "market_context.hpp"
#include "papertrader.hpp"
#include "strategy.hpp"
#include <iostream>
#include <vector>

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "Bitte CSV-Datei angeben!" << std::endl;
    return 1;
  }

  // --- 1. ZENTRALE KONFIGURATION ---
  EngineConfig config;
  config.tick_size = 1.0; // 1$ pro Tick-Level
  config.stop_loss_pct = 0.005;
  config.take_profit_pct = 0.01;

  // --- 2. KOMPONENTEN INITIALISIEREN ---
  Aggregator aggregator(config.interval_ms, config.tick_size);
  PaperTrader ptrader(config.stop_loss_pct, config.take_profit_pct);
  ImbalanceStrategy strategy(config.imbalance_threshold, 3);
  std::vector<Bar> history;
  Bar live_bar;
  MarketContext context{history, live_bar};

  std::cout << "Starte Backtest..." << std::endl;

  // --- 3. DER FESTER DATEN-FLOW (Das Lambda-Callback) ---
  size_t processed_ticks =
      CSVLoader::ProcessBinanceCSV(argv[1], [&](const TradeEvent &trade) {
        // A) Prio 1: Risiko-Management prüfen (Stop Loss / Take Profit bei
        // jedem Tick)
        ptrader.CheckRisk(trade.price);

        // B) Prio 2: Aggregator füttern
        bool candle_finished =
            aggregator.ProcessTrade(live_bar, trade, history);

        // C) Prio 3: Strategie und Ausführung
        if (candle_finished) {
          // Kerze ist geschlossen: Haupt-Signal generieren
          eSignal signal = strategy.OnCandleClose(context);
          if (signal != eSignal::NONE) {
            ptrader.ProcessSignal(signal, live_bar.close);
          }
        } else {
          // Kerze läuft noch: Live-Tick Signal (z.B. für sofortige
          // Imbalance-Reaktion)
          eSignal tick_signal = strategy.OnTickUpdate(context);
          if (tick_signal != eSignal::NONE) {
            ptrader.ProcessSignal(tick_signal, trade.price);
          }
        }
      });

  // --- 4. ABSCHLUSS ---
  std::cout << "Verarbeitete Ticks: " << processed_ticks << std::endl;

  ptrader.CloseOpenPositionAtEnd(live_bar.close);
  ptrader.PrintResults();

  return 0;
}
