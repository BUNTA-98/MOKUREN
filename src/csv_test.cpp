#include "aggregator.hpp"
#include "csv_parser.hpp"
#include "engine_config.hpp"
#include "market_context.hpp"
#include "papertrader.hpp"
#include "strategy.hpp"
#include "ui.hpp"
#include <iostream>

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "Bitte CSV-Datei angeben!" << std::endl;
    return 1;
  }

  // --- 1. ZENTRALE KONFIGURATION ---
  EngineConfig config;
  config.interval_ms = 60000; // 1 Minute in ms
  config.tick_size = 1.0;
  config.stop_loss_pct = 0.005;
  config.take_profit_pct = 0.01;

  // --- 2. KOMPONENTEN INITIALISIEREN ---
  Aggregator aggregator(config.interval_ms, config.tick_size);
  PaperTrader ptrader(config.stop_loss_pct, config.take_profit_pct);
  ImbalanceStrategy strategy(config.imbalance_threshold, 3);
  Bar live_bar;

  int64_t first_timestamp = 0;
  int64_t last_timestamp = 0;

  std::cout << "Starte Backtest..." << std::endl;

  // --- 3. DER FESTE DATEN-FLOW ---
  size_t processed_ticks =
      CSVLoader::ProcessBinanceCSV(argv[1], [&](const TradeEvent &trade) {
        if (first_timestamp == 0) {
          first_timestamp = trade.timestamp;
        }
        last_timestamp = trade.timestamp;

        ptrader.CheckRisk(trade.price);
        bool candle_finished = aggregator.ProcessTrade(live_bar, trade);

        MarketContext context{aggregator.GetHistory(), live_bar};

        if (candle_finished) {
          eSignal signal = strategy.OnCandleClose(context);
          if (signal != eSignal::NONE) {
            ptrader.ProcessSignal(signal, live_bar.close);
          }
        } else {
          eSignal tick_signal = strategy.OnTickUpdate(context);
          if (tick_signal != eSignal::NONE) {
            ptrader.ProcessSignal(tick_signal, trade.price);
          }
        }
      });

  aggregator.FlushLastCandle(live_bar);

  // --- 4. DIAGNOSE AUSGABE ---
  int64_t total_duration_sec = (last_timestamp - first_timestamp) / 1000;
  std::cout << "[DIAGNOSE] CSV Zeitspanne: " << total_duration_sec
            << " Sekunden (" << (total_duration_sec / 60) << " Minuten)"
            << std::endl;

  ptrader.CloseOpenPositionAtEnd(live_bar.close);
  ptrader.PrintResults();

  std::cout << "Verarbeitete Ticks: " << processed_ticks << std::endl;
  std::cout << "[DEBUG] Backtest fertig." << std::endl;
  std::cout << "[DEBUG] Anzahl historischer Kerzen: "
            << aggregator.GetHistory().size() << std::endl;

  if (aggregator.GetHistory().empty()) {
    std::cerr << "[FEHLER] Keine Kerzen generiert!" << std::endl;
    return 1;
  }

  UIManager ui(aggregator.GetHistory(), ptrader);
  ui.Run();

  return 0;
}