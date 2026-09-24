#include "aggregator.hpp"
#include "alpha_engine.hpp"
#include "csv_parser.hpp"
#include "engine_config.hpp"
#include "market_context.hpp"
#include "papertrader.hpp"
#include "position_sizer.hpp"
#include "risk_engine.hpp"
#include "strategy.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// speicherstruktur für ergebnisse
struct TestResult {
  double delta;
  double sl_pct;
  double tp_pct;
  double net_profit;
  int trades;
  double winrate;
};

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "csv required" << std::endl; //[cite: 5]
    return 1;
  }

  // basis config
  EngineConfig base_config;
  base_config.interval_ms = 60000; //[cite: 5]
  base_config.tick_size = 1.0;     //[cite: 5]

  // 1. dateien sammeln und sortieren
  std::vector<std::string> csv_files;
  fs::path input_path = argv[1];

  if (fs::is_directory(input_path)) {
    for (const auto &entry : fs::directory_iterator(input_path)) {
      if (entry.path().extension() == ".csv") {
        csv_files.push_back(entry.path().string());
      }
    }
    std::sort(csv_files.begin(), csv_files.end()); //[cite: 5]
  } else if (fs::is_regular_file(input_path)) {
    csv_files.push_back(input_path.string()); //[cite: 5]
  } else {
    std::cerr << "ungültiger pfad!" << std::endl; //[cite: 5]
    return 1;
  }

  // 2. alle daten in den ram laden (für highspeed loops)
  std::cout << "lade ticks in arbeitsspeicher..." << std::endl;
  std::vector<TradeEvent> all_trades;
  for (const auto &file : csv_files) {
    CSVLoader::ProcessBinanceCSV(
        file, [&](const TradeEvent &trade) { all_trades.push_back(trade); });
  }
  std::cout << "ticks geladen: " << all_trades.size() << "\n\n";

  // 3. grid search vorbereiten
  std::vector<TestResult> results;
  std::cout << "starte parameter-scan...\n";

  // loops für delta, stop-loss und take-profit
  for (double delta = 0.5; delta <= 5.0; delta += 0.1) {
    for (double sl_pct = 0.002; sl_pct <= 0.010; sl_pct += 0.002) {
      for (double tp_pct = 0.005; tp_pct <= 0.020; tp_pct += 0.005) {

        // module für jeden lauf frisch instanziieren[cite: 5]
        Aggregator aggregator(base_config.interval_ms,
                              base_config.tick_size); //[cite: 5]
        PaperTrader ptrader(sl_pct, tp_pct);          //[cite: 5]

        DeltaAbsorptionTrigger delta_trigger(delta);
        MinVolumeFilter vol_filter(1.0); //[cite: 5]

        PipelineStrategy pipeline(&delta_trigger); //[cite: 5]
        pipeline.AddFilter(&vol_filter);           //[cite: 5]
        AlphaEngine alpha(pipeline);               //[cite: 5]

        PositionSizer sizer(ptrader, 0.01,
                            10.0); // 1% risk, min 10$ distance[cite: 5]

        SinglePositionLock pos_lock(ptrader);    //[cite: 5]
        MaxDrawdownLock dd_lock(ptrader, 100.0); // max 100$ loss[cite: 5]
        MaxLeverageLock lev_lock(ptrader, 10.0); // max 3x hebel[cite: 5]

        RiskManager risk_manager;
        risk_manager.AddModule(&pos_lock); //[cite: 5]
        //risk_manager.AddModule(&dd_lock);  //[cite: 5]
        risk_manager.AddModule(&lev_lock); //[cite: 5]

        Bar live_bar; //[cite: 5]

        // hochgeschwindigkeits-simulation aus dem ram
        for (const auto &trade : all_trades) {
          ptrader.CheckRisk(trade.price); //[cite: 5]
          bool candle_finished =
              aggregator.ProcessTrade(live_bar, trade);             //[cite: 5]
          MarketContext context{aggregator.GetHistory(), live_bar}; //[cite: 5]

          TradeSignal raw_signal =
              alpha.Evaluate(context, candle_finished); //[cite: 5]

          // ticket mit preisen füllen[cite: 5]
          if (raw_signal.direction != SignalDirection::NONE) {
            raw_signal.entry_price = trade.price; //[cite: 5]

            if (raw_signal.direction == SignalDirection::BUY) {      //[cite: 5]
              raw_signal.stop_loss = trade.price * (1.0 - sl_pct);   //[cite: 5]
              raw_signal.take_profit = trade.price * (1.0 + tp_pct); //[cite: 5]
            } else {
              raw_signal.stop_loss = trade.price * (1.0 + sl_pct);   //[cite: 5]
              raw_signal.take_profit = trade.price * (1.0 - tp_pct); //[cite: 5]
            }
          }

          TradeSignal sized_signal =
              sizer.CalculateSize(raw_signal); //[cite: 5]
          TradeSignal final_signal =
              risk_manager.Evaluate(sized_signal); //[cite: 5]

          if (final_signal.direction != SignalDirection::NONE) { //[cite: 5]
            ptrader.ProcessSignal(final_signal, trade.price);    //[cite: 5]
          }
        }

        // abschluss dieses durchlaufs
        aggregator.FlushLastCandle(live_bar);           //[cite: 5]
        ptrader.CloseOpenPositionAtEnd(live_bar.close); //[cite: 5]

        // ergebnis speichern (ignoriert läufe ohne trades)
        if (ptrader.GetTotalTrades() > 0) {
          results.push_back({delta, sl_pct, tp_pct, ptrader.GetNetProfit(),
                             ptrader.GetTotalTrades(), ptrader.GetWinrate()});
        }
      }
    }
  }

  // 4. sortieren nach höchstem netto-profit
  std::sort(results.begin(), results.end(),
            [](const TestResult &a, const TestResult &b) {
              return a.net_profit > b.net_profit;
            });

  // 5. top 10 tabelle ausgeben
  std::cout << "\n=== top 10 parameter-kombinationen ===\n";
  std::cout << "delta\tsl(%)\ttp(%)\ttrades\twinrate\tprofit(usdt)\n";
  std::cout << "--------------------------------------------------------\n";

  int limit = std::min(static_cast<int>(results.size()), 10);
  for (int i = 0; i < limit; i++) {
    const auto &r = results[i];
    std::printf("%.1f\t%.1f%%\t%.1f%%\t%d\t%.1f%%\t%.2f\n", r.delta,
                (r.sl_pct * 100.0), (r.tp_pct * 100.0), r.trades, r.winrate,
                r.net_profit);
  }

  return 0; //[cite: 5]
}
