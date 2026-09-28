#pragma once
#include "config_parser.hpp"
#include "csv_parser.hpp"
#include "factory.hpp"
#include "market_context.hpp"
#include "ui_footprint.hpp"
#include "replay_manager.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;



struct TestResult {
  std::map<std::string, double> parameters;
  nlohmann::json full_config; 
  double net_profit;
  double max_drawdown; 
  int trades;
  double winrate;
  std::vector<TradeRecord> trade_log;
};


struct WFAWindow {
  int64_t in_sample_start;
  int64_t in_sample_end;
  int64_t out_of_sample_start;
  int64_t out_of_sample_end;
};



class Mokuren {

private:
  
  std::vector<TradeEvent> LoadAllTrades(const std::string &input_path_str) {
    fs::path input_path = input_path_str;
    fs::path cache_path;

    if (fs::is_directory(input_path)) {
      std::string dir_name = input_path.filename().string();
      if (dir_name.empty()) dir_name = "folder";
      cache_path = input_path / (dir_name + "_cache.bin");
    } else {
      std::string file_stem = input_path.stem().string();
      cache_path = input_path.parent_path() / (file_stem + "_cache.bin");
    }

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

    if (!all_trades.empty()) {
      std::ofstream cache_file(cache_path, std::ios::binary);
      if (cache_file) {
        cache_file.write(reinterpret_cast<const char*>(all_trades.data()), 
                         all_trades.size() * sizeof(TradeEvent));
      }
    }
    return all_trades;
  }



std::vector<TradeEvent> SliceTrades(const std::vector<TradeEvent>& source_trades, int64_t start_time, int64_t end_time) {
    std::vector<TradeEvent> sliced;
    sliced.reserve(source_trades.size() / 4); // RAM-Optimierung

    for (const auto& trade : source_trades) {
      if (trade.timestamp >= start_time && trade.timestamp <= end_time) {
        sliced.push_back(trade);
      }
      // Da Ticks chronologisch sind, können wir sofort abbrechen, wenn wir das Fenster verlassen!
      if (trade.timestamp > end_time) break; 
    }
    return sliced;
  }



  std::vector<WFAWindow> GenerateWFAWindows(int64_t first_ts, int64_t last_ts, 
                                            int64_t in_sample_ms, int64_t out_of_sample_ms, 
                                            int64_t step_ms) {
    std::vector<WFAWindow> windows;
    int64_t current_start = first_ts;

    while (true) {
      WFAWindow win;
      win.in_sample_start = current_start;
      win.in_sample_end = current_start + in_sample_ms - 1;
      
      win.out_of_sample_start = win.in_sample_end + 1;
      win.out_of_sample_end = win.out_of_sample_start + out_of_sample_ms - 1;

      // Wenn das Test-Fenster über uer tatsächliches Datenende hinausschießt, brechen wir ab.
      // So stellen wir sicher, dass wir nur komplette, saubere Fenster testen.
      if (win.out_of_sample_end > last_ts) {
        break; 
      }

      windows.push_back(win);
      current_start += step_ms; // Das Fenster für den nächsten Durchlauf nach vorne schieben
    }
    
    return windows;
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
    
    nlohmann::json base_json = nlohmann::json::parse(file);
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
    std::atomic<size_t> task_index{0};
    std::atomic<int> current_iteration{0};

    std::vector<std::thread> workers;
    for (unsigned int i = 0; i < max_cores; ++i) {
      workers.emplace_back([&, this]() {
        while (true) {
          size_t idx = task_index.fetch_add(1);
          if (idx >= runs.size()) break; 
          
          const auto& run = runs[idx];
          AppConfig cfg = AppConfig::Load(run.full_json);
          EngineInstance eng = StrategyFactory::Build(cfg);

          // safe inject trade management config into paper trader
          auto* pt = dynamic_cast<PaperTrader*>(eng.ptrader.get());
          if (pt) {
            pt->ApplyManagementConfig(
                cfg.tm_config.be_trigger_pct, cfg.tm_config.be_target_pct,
                cfg.tm_config.enable_trailing, cfg.tm_config.trailing_trigger_pct, cfg.tm_config.trailing_dist_pct,
                cfg.tm_config.enable_scale_out, cfg.tm_config.scale_out_trigger_pct, cfg.tm_config.scale_out_fraction
            );
          }

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

          if (eng.ptrader->GetTotalTrades() > 0) {
            TestResult res;
            res.parameters = run.grid_values;
            res.full_config = run.full_json; 
            res.net_profit = eng.ptrader->GetNetProfit();
            res.max_drawdown = eng.ptrader->GetMaxDrawdown(); 
            res.trades = eng.ptrader->GetTotalTrades();
            res.winrate = eng.ptrader->GetWinrate();
            res.trade_log = eng.ptrader->GetTradeHistory();
            on_result(res);
          }
          
          int done = ++current_iteration;
          on_progress(done, total_iterations);
        }
      });
    }

    for (auto& w : workers) {
      if (w.joinable()) w.join();
    }
    
    on_status("SCAN COMPLETE");
  }

  void RunWFA(const std::string &filepath, const std::string &config_path,
              std::function<void(const std::string&)> on_status) {

    on_status("LOADING TICKS FOR WFA...");
    std::vector<TradeEvent> all_trades = LoadAllTrades(filepath);
    if (all_trades.empty()) {
      on_status("ERROR: NO TICK DATA");
      return;
    }

    int64_t first_ts = all_trades.front().timestamp;
    int64_t last_ts = all_trades.back().timestamp;

    // Zeitfenster-Setup: 90 Tage Training, 30 Tage Test, 30 Tage rollieren (Step)
    int64_t day_ms = 86400000LL; 
    int64_t in_sample_ms = 90 * day_ms;
    int64_t out_of_sample_ms = 30 * day_ms;
    int64_t step_ms = 30 * day_ms;

    std::vector<WFAWindow> windows = GenerateWFAWindows(first_ts, last_ts, in_sample_ms, out_of_sample_ms, step_ms);
    on_status("WFA: GENERATED " + std::to_string(windows.size()) + " ROLLING WINDOWS");

    for (size_t i = 0; i < windows.size(); ++i) {
      const auto& win = windows[i];
      on_status("WFA WINDOW " + std::to_string(i + 1) + "/" + std::to_string(windows.size()) + " - SLICING DATA...");

      // 1. Daten präzise für dieses Fenster zerschneiden
      std::vector<TradeEvent> in_sample_data = SliceTrades(all_trades, win.in_sample_start, win.in_sample_end);
      std::vector<TradeEvent> out_of_sample_data = SliceTrades(all_trades, win.out_of_sample_start, win.out_of_sample_end);

      // 2. CONFIGS GENERIEREN
      std::ifstream file(config_path);
      if (!file.is_open()) continue;
      nlohmann::json base_json = nlohmann::json::parse(file);
      std::vector<RunConfig> runs = GridScanner::GenerateGrid(base_json);

      double best_in_sample_profit = -999999.0;
      RunConfig best_config = runs[0];

      // 3. IN-SAMPLE TRAINING (Grid Search auf Trainingsdaten)
      for (const auto& run : runs) {
        AppConfig cfg = AppConfig::Load(run.full_json);
        EngineInstance eng = StrategyFactory::Build(cfg);

        // Management Config injizieren (genau wie in RunGridSearch)
        auto* pt = dynamic_cast<PaperTrader*>(eng.ptrader.get());
        if (pt) {
          pt->ApplyManagementConfig(
              cfg.tm_config.be_trigger_pct, cfg.tm_config.be_target_pct,
              cfg.tm_config.enable_trailing, cfg.tm_config.trailing_trigger_pct, cfg.tm_config.trailing_dist_pct,
              cfg.tm_config.enable_scale_out, cfg.tm_config.scale_out_trigger_pct, cfg.tm_config.scale_out_fraction
          );
        }

        Bar live_bar, htf_bar;
        for (const auto &trade : in_sample_data) {
          eng.pos_manager->Update(trade.price);
          eng.ptrader->CheckRisk(trade.price, trade.timestamp);
          bool candle_finished = eng.aggregator->ProcessTrade(live_bar, trade);
          eng.htf_aggregator->ProcessTrade(htf_bar, trade);
          MarketContext context{eng.aggregator->GetHistory(), live_bar, htf_bar};

          TradeSignal raw_signal = eng.alpha->Evaluate(context, candle_finished);
          if (raw_signal.direction != SignalDirection::NONE) {
            raw_signal.entry_price = trade.price;
            raw_signal.stop_loss = trade.price * (raw_signal.direction == SignalDirection::BUY ? (1.0 - cfg.sl_pct) : (1.0 + cfg.sl_pct));
            raw_signal.take_profit = trade.price * (raw_signal.direction == SignalDirection::BUY ? (1.0 + cfg.tp_pct) : (1.0 - cfg.tp_pct));
          }
          TradeSignal sized = eng.sizer->CalculateSize(raw_signal);
          TradeSignal final_sig = eng.risk_manager->Evaluate(sized, trade.timestamp);
          if (final_sig.direction != SignalDirection::NONE) {
            eng.ptrader->ProcessSignal(final_sig, trade.price, trade.timestamp);
          }
        }
        eng.aggregator->FlushLastCandle(live_bar);
        eng.ptrader->CloseOpenPositionAtEnd(live_bar.close, live_bar.timestamp_start);

        double profit = eng.ptrader->GetNetProfit();
        if (profit > best_in_sample_profit) {
          best_in_sample_profit = profit;
          best_config = run;
        }
      }

      // 4. OUT-OF-SAMPLE VALIDIERUNG (Den Sieger auf ungesehene Testdaten loslassen)
      AppConfig oos_cfg = AppConfig::Load(best_config.full_json);
      EngineInstance oos_eng = StrategyFactory::Build(oos_cfg);
      auto* oos_pt = dynamic_cast<PaperTrader*>(oos_eng.ptrader.get());
      if (oos_pt) {
        oos_pt->ApplyManagementConfig(
            oos_cfg.tm_config.be_trigger_pct, oos_cfg.tm_config.be_target_pct,
            oos_cfg.tm_config.enable_trailing, oos_cfg.tm_config.trailing_trigger_pct, oos_cfg.tm_config.trailing_dist_pct,
            oos_cfg.tm_config.enable_scale_out, oos_cfg.tm_config.scale_out_trigger_pct, oos_cfg.tm_config.scale_out_fraction
        );
      }

      Bar oos_live, oos_htf;
      for (const auto &trade : out_of_sample_data) {
        oos_eng.pos_manager->Update(trade.price);
        oos_eng.ptrader->CheckRisk(trade.price, trade.timestamp);
        bool candle_finished = oos_eng.aggregator->ProcessTrade(oos_live, trade);
        oos_eng.htf_aggregator->ProcessTrade(oos_htf, trade);
        MarketContext context{oos_eng.aggregator->GetHistory(), oos_live, oos_htf};

        TradeSignal raw_signal = oos_eng.alpha->Evaluate(context, candle_finished);
        if (raw_signal.direction != SignalDirection::NONE) {
          raw_signal.entry_price = trade.price;
          raw_signal.stop_loss = trade.price * (raw_signal.direction == SignalDirection::BUY ? (1.0 - oos_cfg.sl_pct) : (1.0 + oos_cfg.sl_pct));
          raw_signal.take_profit = trade.price * (raw_signal.direction == SignalDirection::BUY ? (1.0 + oos_cfg.tp_pct) : (1.0 - oos_cfg.tp_pct));
        }
        TradeSignal sized = oos_eng.sizer->CalculateSize(raw_signal);
        TradeSignal final_sig = oos_eng.risk_manager->Evaluate(sized, trade.timestamp);
        if (final_sig.direction != SignalDirection::NONE) {
          oos_eng.ptrader->ProcessSignal(final_sig, trade.price, trade.timestamp);
        }
      }
      oos_eng.aggregator->FlushLastCandle(oos_live);
      oos_eng.ptrader->CloseOpenPositionAtEnd(oos_live.close, oos_live.timestamp_start);

      double oos_profit = oos_eng.ptrader->GetNetProfit();
      on_status("WFA WINDOW " + std::to_string(i + 1) + " OOS PROFIT: $" + std::to_string(oos_profit));
    }

    on_status("WFA COMPLETE");
  }

  ReplayResult ReplaySingleRun(const std::string &filepath, const nlohmann::json& winning_config) {
      std::vector<TradeEvent> all_trades = LoadAllTrades(filepath);
      return ReplayManager::Run(all_trades, winning_config);
  }
};
