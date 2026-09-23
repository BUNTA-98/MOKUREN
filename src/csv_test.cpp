#include "aggregator.hpp"
#include "alpha_engine.hpp"
#include "csv_parser.hpp"
#include "engine_config.hpp"
#include "market_context.hpp"
#include "papertrader.hpp"
#include "strategy.hpp"
#include "ui.hpp"

#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

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

  // strategy building

  // trigger
  // StackedImbalanceTrigger imb_trigger(config.imbalance_threshold, 3);
  DeltaAbsorptionTrigger delta_trigger(10.0);

  /*
  AND_Trigger multi_trigger;
  multi_trigger.AddTrigger(&imb_trigger);
  multi_trigger.AddTrigger(&delta_trigger);
  */

  // filter
  MinVolumeFilter my_volume_filter(10.0);
  POCTrendFilter poc_filter;

  // build pipeline
  PipelineStrategy pipeline(&delta_trigger);
  pipeline.AddFilter(&my_volume_filter);
  // pipeline.AddFilter(&poc_filter);

  AlphaEngine alpha(pipeline);

  Bar live_bar;

  int64_t first_timestamp = 0;
  int64_t last_timestamp = 0;

  std::cout << "Starte Backtest..." << std::endl;

  // multithreading
  std::mutex data_mutex;
  size_t processed_ticks = 0; // Nach oben ziehen, für die spätere Ausgabe

  // background thread
  std::thread backend_thread([&]() {
    processed_ticks =
        CSVLoader::ProcessBinanceCSV(argv[1], [&](const TradeEvent &trade) {
          { // <-- START MUTEX SCOPE
            std::lock_guard<std::mutex> lock(data_mutex); // Schlüssel holen

            if (first_timestamp == 0) {
              first_timestamp = trade.timestamp;
            }
            last_timestamp = trade.timestamp;

            ptrader.CheckRisk(trade.price);
            bool candle_finished = aggregator.ProcessTrade(live_bar, trade);

            MarketContext context{aggregator.GetHistory(), live_bar};

            // alpha
            eSignal final_signal = alpha.Evaluate(context, candle_finished);

            // 5. EXECUTION ENGINE (Später schalten wir hier die RiskEngine
            // dazwischen!)
            if (final_signal != eSignal::NONE) {
              ptrader.ProcessSignal(final_signal, trade.price);
            }

          } // ENDE MUTEX

          // Künstliche Verzögerung, damit die UI flüssig im Takt rendert
          // std::this_thread::sleep_for(std::chrono::microseconds(100));
        });

    // Letzte Kerze sichern (ebenfalls mit Mutex absichern!)
    {
      std::lock_guard<std::mutex> lock(data_mutex);
      aggregator.FlushLastCandle(live_bar);
      ptrader.CloseOpenPositionAtEnd(live_bar.close);
    }
  });

  // --- 4. UI IM MAIN-THREAD ---
  // Wichtig: Der Mutex wird jetzt an den UIManager übergeben
  UIManager ui(aggregator.GetHistory(), ptrader, data_mutex);
  ui.Run();

  // after close
  if (backend_thread.joinable()) {
    backend_thread.join();
  }

  int64_t total_duration_sec = (last_timestamp - first_timestamp) / 1000;
  std::cout << "[DIAGNOSE] CSV Zeitspanne: " << total_duration_sec
            << " Sekunden (" << (total_duration_sec / 60) << " Minuten)"
            << std::endl;

  ptrader.PrintResults();

  std::cout << "Verarbeitete Ticks: " << processed_ticks << std::endl;
  std::cout << "[DEBUG] Backtest fertig." << std::endl;
  std::cout << "[DEBUG] Anzahl historischer Kerzen: "
            << aggregator.GetHistory().size() << std::endl;

  return 0;
}
