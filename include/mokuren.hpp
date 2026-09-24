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
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <fstream>

namespace fs = std::filesystem;

// result storage
struct TestResult {
  double delta;
  double sl_pct;
  double tp_pct;
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
    AppConfig cfg = AppConfig::Load(config_path);

    std::cout << "--- CONFIG CHECK ---\n"
          << "interval_ms: " << cfg.interval_ms << "\n"
          << "macro_ms:    " << cfg.macro_interval_ms << "\n"
          << "delta_step:  " << cfg.d_step << "\n"
          << "--------------------\n";
    
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

    // get available cpu cores
    unsigned int max_cores_req = std::thread::hardware_concurrency();
    // cpu cores from config
    unsigned int max_cores = cfg.max_cores;
    if (max_cores > max_cores_req){max_cores = max_cores_req;}
    std::cout << "using " << max_cores << " parallel threads...\n";
    std::cout << "start parameter scan (multithreaded)...\n";

    // null-step fix 
    double d_step = cfg.d_step == 0 ? 1 : cfg.d_step;
    double sl_step = cfg.sl_step == 0 ? 1 : cfg.sl_step;
    double tp_step = cfg.tp_step == 0 ? 1 : cfg.tp_step;

    // count iterations for progress bar
    int total_iterations = 0;
    for (double d = cfg.d_min; d <= cfg.d_max; d += d_step) {
      for (double sl = cfg.sl_min; sl <= cfg.sl_max; sl += sl_step) {
        for (double tp = cfg.tp_min; tp <= cfg.tp_max; tp += tp_step) {
          total_iterations++;
        }
      }
    }

    std::atomic<int> current_iteration{0};

    // multithreading loop
    for (double delta = cfg.d_min; delta <= cfg.d_max; delta += d_step) {
      for (double sl = cfg.sl_min; sl <= cfg.sl_max; sl += sl_step) {
        for (double tp = cfg.tp_min; tp <= cfg.tp_max; tp += tp_step) {

          futures.push_back(std::async(std::launch::async, [=, &all_trades, &results, &results_mutex, &console_mutex, &current_iteration, &cfg, this]() {
            
            // init full engine via factory
            EngineInstance eng = StrategyFactory::Build(cfg, delta, sl, tp);

            Bar live_bar;
            Bar htf_bar;

            // tick loop
            for (const auto &trade : all_trades) {
              eng.pos_manager->Update(trade.price);
              
              // pass timestamp to risk check
              eng.ptrader->CheckRisk(trade.price, trade.timestamp);

              bool candle_finished = eng.aggregator->ProcessTrade(live_bar, trade);
              eng.htf_aggregator->ProcessTrade(htf_bar, trade);
              MarketContext context{eng.aggregator->GetHistory(), live_bar, htf_bar};

              TradeSignal raw_signal = eng.alpha->Evaluate(context, candle_finished);

              if (raw_signal.direction != SignalDirection::NONE) {
                raw_signal.entry_price = trade.price;
                if (raw_signal.direction == SignalDirection::BUY) {
                  raw_signal.stop_loss = trade.price * (1.0 - sl);
                  raw_signal.take_profit = trade.price * (1.0 + tp);
                } else {
                  raw_signal.stop_loss = trade.price * (1.0 + sl);
                  raw_signal.take_profit = trade.price * (1.0 - tp);
                }
              }

              TradeSignal sized_signal = eng.sizer->CalculateSize(raw_signal);
              TradeSignal final_signal = eng.risk_manager->Evaluate(sized_signal);

              if (final_signal.direction != SignalDirection::NONE) {
                // pass timestamp to signal processing
                eng.ptrader->ProcessSignal(final_signal, trade.price, trade.timestamp);
              }
            }

            eng.aggregator->FlushLastCandle(live_bar);
            
            // pass end time
            eng.ptrader->CloseOpenPositionAtEnd(live_bar.close, live_bar.timestamp_start);

            // save results thread-safe
            if (eng.ptrader->GetTotalTrades() > 0) {
              std::lock_guard<std::mutex> lock(results_mutex);
              results.push_back({delta, sl, tp, eng.ptrader->GetNetProfit(),
                                 eng.ptrader->GetTotalTrades(), eng.ptrader->GetWinrate(),
                                 eng.ptrader->GetTradeHistory()});
            }
            
            // draw red progress bar thread-safe
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
      }
    }

    // wait for remaining threads
    for (auto &f : futures) f.get();
    std::cout << "\n\n";

    // sort and print
    std::sort(results.begin(), results.end(),
              [](const TestResult &a, const TestResult &b) {
                return a.net_profit > b.net_profit;
              });

    std::cout << "=== top 10 parameter combinations ===\n";
    std::cout << "delta\tsl(%)\ttp(%)\ttrades\twinrate\tprofit(usdt)\n";
    std::cout << "--------------------------------------------------------\n";

    int limit = std::min(static_cast<int>(results.size()), 10);
    for (int i = 0; i < limit; i++) {
      const auto &r = results[i];
      std::printf("%.2f\t%.1f%%\t%.1f%%\t%d\t%.1f%%\t%.2f\n", r.delta,
                  (r.sl_pct * 100.0), (r.tp_pct * 100.0), r.trades, r.winrate,
                  r.net_profit);
    }

    // export top 10 runs with timestamps
    if (!results.empty()) {
      std::ofstream file("top_10_runs_trades.csv");
      file << "Rank,Delta,SL_Pct,TP_Pct,EntryTime,ExitTime,Type,EntryPrice,ExitPrice,NetProfit,Reason\n";
      
      int log_limit = std::min(static_cast<int>(results.size()), 10);
      for (int i = 0; i < log_limit; i++) {
        const auto &run = results[i];
        
        for (const auto& t : run.trade_log) {
          file << (i + 1) << ","                    
               << run.delta << ","                  
               << (run.sl_pct * 100.0) << ","       
               << (run.tp_pct * 100.0) << ","
               << t.entry_time << ","
               << t.exit_time << ","
               << (t.direction == SignalDirection::BUY ? "LONG" : "SHORT") << ","
               << t.entry_price << "," 
               << t.exit_price << "," 
               << t.net_profit << "," 
               << t.exit_reason << "\n";
        }
      }
      std::cout << "\n-> trade-logs exported to top_10_runs_trades.csv\n";
    }
  }
};