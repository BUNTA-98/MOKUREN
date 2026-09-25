#pragma once
#include "config_parser.hpp"
#include "csv_parser.hpp"
#include "factory.hpp"
#include "market_context.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <fstream>

namespace fs = std::filesystem;

// result storage
struct TestResult {
  std::map<std::string, double> parameters;
  double net_profit;
  int trades;
  double winrate;
  std::vector<TradeRecord> trade_log;
};

class Mokuren {
private:
  // load data into ram
  std::vector<TradeEvent> LoadAllTrades(const std::string &input_path_str) {
    std::vector<std::string> csv_files;
    fs::path input_path = input_path_str;

    if (fs::is_directory(input_path)) {
      for (const auto &entry : fs::directory_iterator(input_path)) {
        if (entry.path().extension() == ".csv") {
          csv_files.push_back(entry.path().string());
        }
      }
      std::sort(csv_files.begin(), csv_files.end());
    } else if (fs::is_regular_file(input_path)) {
      csv_files.push_back(input_path.string());
    } else {
      std::cerr << "invalid path\n";
      return {};
    }

    std::vector<TradeEvent> all_trades;
    for (const auto &file : csv_files) {
      CSVLoader::ProcessBinanceCSV(
          file, [&](const TradeEvent &trade) { all_trades.push_back(trade); });
    }
    return all_trades;
  }

public:
  Mokuren() = default;

  // async grid search
  void RunGridSearch(const std::string &filepath, const std::string &config_path) {
    std::ifstream file(config_path);
    if (!file.is_open()) {
      std::cerr << "error: config.json not found!\n";
      return;
    }
    
    json base_json = json::parse(file);
    std::vector<RunConfig> runs = GridScanner::GenerateGrid(base_json);
    
    std::cout << "--- DYNAMIC GRID SEARCH ---\n";
    std::cout << "generated " << runs.size() << " combinations to test.\n";
    std::cout << "loading ticks into ram...\n";
    
    std::vector<TradeEvent> all_trades = LoadAllTrades(filepath);
    if (all_trades.empty()) {
      std::cerr << "no data found. aborting.\n";
      return;
    }
    std::cout << "ticks loaded: " << all_trades.size() << "\n\n";

    std::vector<TestResult> results;
    std::mutex results_mutex;
    std::mutex console_mutex;
    std::vector<std::future<void>> futures;

    unsigned int max_cores = base_json["environment"].value("max_cores", 0);
    if (max_cores == 0) max_cores = std::thread::hardware_concurrency();
    
    std::cout << "using " << max_cores << " parallel threads...\n";
    std::cout << "start parameter scan (multithreaded)...\n";

    int total_iterations = runs.size();
    std::atomic<int> current_iteration{0};

    // multithreading loop
    for (const auto& run : runs) {
      futures.push_back(std::async(std::launch::async, [=, &all_trades, &results, &results_mutex, &console_mutex, &current_iteration, this]() {
        
        AppConfig cfg = AppConfig::Load(run.full_json);
        EngineInstance eng = StrategyFactory::Build(cfg);

        Bar live_bar;
        Bar htf_bar;

        // tick loop
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

        // save results thread-safe
        if (eng.ptrader->GetTotalTrades() > 0) {
          std::lock_guard<std::mutex> lock(results_mutex);
          results.push_back({run.grid_values, eng.ptrader->GetNetProfit(),
                             eng.ptrader->GetTotalTrades(), eng.ptrader->GetWinrate(),
                             eng.ptrader->GetTradeHistory()});
        }
        
        // draw progress bar
        int done = ++current_iteration;
        {
          std::lock_guard<std::mutex> lock(console_mutex);
          int bar_width = 50;
          float progress = static_cast<float>(done) / total_iterations;
          int pos = static_cast<int>(bar_width * progress);

          std::cout << "\r"; 
          for (int i = 0; i < bar_width; ++i) {
            if (i < pos) std::cout << "\033[38;2;255;0;0m█";
            else std::cout << "\033[38;2;40;40;40m█";
          }
          std::cout << "\033[0m " << static_cast<int>(progress * 100.0) << " %" << std::flush;
        }
      }));

      // throttle threads to prevent oom
      if (futures.size() >= max_cores) {
        for (auto &f : futures) f.get();
        futures.clear();
      }
    }

    // wait for remaining threads
    for (auto &f : futures) f.get();
    std::cout << "\n\n";

    // sort by profit
    std::sort(results.begin(), results.end(), [](const TestResult &a, const TestResult &b) {
      return a.net_profit > b.net_profit;
    });

    // print top 10 to terminal
    std::cout << "=== top 10 parameter combinations ===\n";
    
    int limit = std::min(static_cast<int>(results.size()), 10);
    for (int i = 0; i < limit; i++) {
      const auto &r = results[i];
      
      std::string param_str;
      for (const auto& [k, v] : r.parameters) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", v); // trim trailing zeros
        param_str += k + "=" + std::string(buf) + " ";
      }
      
      std::printf("profit: %8.2f | winrate: %4.1f%% | trades: %3d | params: %s\n", 
                  r.net_profit, r.winrate, r.trades, param_str.c_str());
    }

    // export top 10 to csv
    if (!results.empty()) {
      std::ofstream file("runs.csv");
      file << "Rank,Parameters,EntryTime,ExitTime,Type,EntryPrice,ExitPrice,NetProfit,Reason\n";
      
      int log_limit = std::min(static_cast<int>(results.size()), 10);
      for (int i = 0; i < log_limit; i++) {
        const auto &run = results[i];
        
        std::string param_str;
        for (const auto& [k, v] : run.parameters) {
          char buf[32];
          snprintf(buf, sizeof(buf), "%g", v); // trim trailing zeros
          param_str += k + "=" + std::string(buf) + ";"; // use semicolon for csv
        }

        for (const auto& t : run.trade_log) {
          file << (i + 1) << ","                    
               << param_str << ","                  
               << t.entry_time << ","
               << t.exit_time << ","
               << (t.direction == SignalDirection::BUY ? "LONG" : "SHORT") << ","
               << t.entry_price << "," 
               << t.exit_price << "," 
               << t.net_profit << "," 
               << t.exit_reason << "\n";
        }
      }
      std::cout << "\n-> trade-logs exported to runs.csv\n";
    }
  }
};