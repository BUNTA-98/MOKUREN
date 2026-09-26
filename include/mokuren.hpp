#pragma once
#include "config_parser.hpp"
#include "csv_parser.hpp"
#include "factory.hpp"
#include "market_context.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional> // wichtig für callbacks
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <fstream>

namespace fs = std::filesystem;

struct TestResult {
  std::map<std::string, double> parameters;
  double net_profit;
  int trades;
  double winrate;
  std::vector<TradeRecord> trade_log;
};

class Mokuren {
private:
  std::vector<TradeEvent> LoadAllTrades(const std::string &input_path_str) {
    fs::path input_path = input_path_str;
    
    // 1. definiere den namen der cache-datei
    fs::path cache_path = input_path;
    if (fs::is_directory(input_path)) {
      cache_path /= "ticks_cache.bin";
    } else {
      cache_path = input_path.parent_path() / "ticks_cache.bin";
    }

    // 2. versuch: lade direkt aus dem binären cache (millisekunden)
    if (fs::exists(cache_path)) {
      std::ifstream cache_file(cache_path, std::ios::binary);
      if (cache_file) {
        cache_file.seekg(0, std::ios::end);
        std::streamsize size = cache_file.tellg();
        cache_file.seekg(0, std::ios::beg);

        size_t count = size / sizeof(TradeEvent);
        std::vector<TradeEvent> all_trades(count);

        if (cache_file.read(reinterpret_cast<char*>(all_trades.data()), size)) {
          return all_trades;
        }
      }
    }

    // 3. fallback: lade die csvs, wenn kein cache da ist
    std::vector<std::string> csv_files;
    if (fs::is_directory(input_path)) {
      for (const auto &entry : fs::directory_iterator(input_path)) {
        if (entry.path().extension() == ".csv") {
          csv_files.push_back(entry.path().string());
        }
      }
      std::sort(csv_files.begin(), csv_files.end());
    } else if (fs::is_regular_file(input_path)) {
      csv_files.push_back(input_path.string());
    }

    std::vector<TradeEvent> all_trades;
    all_trades.reserve(5000000); 

    for (const auto &file : csv_files) {
      CSVLoader::ProcessBinanceCSV(
          file, [&](const TradeEvent &trade) { all_trades.push_back(trade); });
    }

    // 4. cache für den nächsten run speichern
    if (!all_trades.empty()) {
      std::ofstream cache_file(cache_path, std::ios::binary);
      if (cache_file) {
        cache_file.write(reinterpret_cast<const char*>(all_trades.data()), 
                         all_trades.size() * sizeof(TradeEvent));
      }
    }

    return all_trades;
  }

public:
  Mokuren() = default;

  void RunGridSearch(const std::string &filepath, const std::string &config_path,
                     std::function<void(const std::string&)> on_status,
                     std::function<void(int, int)> on_progress,
                     std::function<void(const TestResult&)> on_result) {
                     
    std::ifstream file(config_path);
    if (!file.is_open()) {
      on_status("ERROR: CONFIG NOT FOUND");
      return;
    }
    
    json base_json = json::parse(file);
    std::vector<RunConfig> runs = GridScanner::GenerateGrid(base_json);
    
    on_status("LOADING SECURE TICK STREAM...");
    
    std::vector<TradeEvent> all_trades = LoadAllTrades(filepath);
    if (all_trades.empty()) {
      on_status("ERROR: NO TICK DATA FOUND");
      return;
    }

    unsigned int max_cores = base_json["environment"].value("max_cores", 0);
    if (max_cores == 0) max_cores = std::thread::hardware_concurrency();
    
    on_status("GRID SCAN ACTIVE (" + std::to_string(max_cores) + " THREADS)");

    int total_iterations = runs.size();
    
    // diese beiden variablen steuern den kompletten pool thread-sicher
    std::atomic<size_t> task_index{0};
    std::atomic<int> current_iteration{0};

    // unsterbliche worker aufsetzen
    std::vector<std::thread> workers;
    for (unsigned int i = 0; i < max_cores; ++i) {
      workers.emplace_back([&, this]() {
        while (true) {
          // ziehe die nächste aufgabe vom stapel
          size_t idx = task_index.fetch_add(1);
          
          // wenn der stapel leer ist, beendet sich der worker
          if (idx >= runs.size()) {
            break; 
          }
          
          const auto& run = runs[idx];
          
          // --- ENGINE LOGIK FÜR DIESEN RUN ---
          AppConfig cfg = AppConfig::Load(run.full_json);
          EngineInstance eng = StrategyFactory::Build(cfg);

          Bar live_bar;
          Bar htf_bar;

          for (const auto &trade : all_trades) {
            eng.pos_manager->Update(trade.price);
            eng.ptrader->CheckRisk(trade.price, trade.timestamp);

            bool candle_finished = eng.aggregator->ProcessTrade(live_bar, trade);
            eng.htf_aggregator->ProcessTrade(htf_bar, trade);
            MarketContext context{eng.aggregator->GetHistory(), live_bar, htf_bar};

            TradeSignal raw_signal = eng.alpha->Evaluate(context, candle_finished);

            if (raw_signal.direction != SignalDirection::NONE) {
              raw_signal.entry_price = trade.price;
              if (raw_signal.direction == SignalDirection::BUY) {
                raw_signal.stop_loss = trade.price * (1.0 - cfg.sl_pct);
                raw_signal.take_profit = trade.price * (1.0 + cfg.tp_pct);
              } else {
                raw_signal.stop_loss = trade.price * (1.0 + cfg.sl_pct);
                raw_signal.take_profit = trade.price * (1.0 - cfg.tp_pct);
              }
            }

            TradeSignal sized_signal = eng.sizer->CalculateSize(raw_signal);
            TradeSignal final_signal = eng.risk_manager->Evaluate(sized_signal, trade.timestamp);

            if (final_signal.direction != SignalDirection::NONE) {
              eng.ptrader->ProcessSignal(final_signal, trade.price, trade.timestamp);
            }
          }

          eng.aggregator->FlushLastCandle(live_bar);
          eng.ptrader->CloseOpenPositionAtEnd(live_bar.close, live_bar.timestamp_start);

          // ergebnis an ui senden
          if (eng.ptrader->GetTotalTrades() > 0) {
            TestResult res{run.grid_values, eng.ptrader->GetNetProfit(),
                           eng.ptrader->GetTotalTrades(), eng.ptrader->GetWinrate(),
                           eng.ptrader->GetTradeHistory()};
            on_result(res);
          }
          
          // fortschrittsbalken updaten
          int done = ++current_iteration;
          on_progress(done, total_iterations);
        }
      });
    }

    // warte, bis alle worker ihre arbeit niederlegen (stapel ist leer)
    for (auto& w : workers) {
      if (w.joinable()) {
        w.join();
      }
    }
    
    on_status("SCAN COMPLETE");
  }
};