#pragma once

#include "aggregator.hpp"
#include "alpha_engine.hpp"
#include "csv_parser.hpp"
#include "engine_config.hpp"
#include "market_context.hpp"
#include "papertrader.hpp"
#include "position_manager.hpp"
#include "position_sizer.hpp"
#include "risk_engine.hpp"
#include "strategy.hpp"

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

namespace fs = std::filesystem;

// result storage
struct TestResult {
  double delta;
  double sl_pct;
  double tp_pct;
  double net_profit;
  int trades;
  double winrate;
};

class Mokuren {
private:
  EngineConfig base_config;

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
  Mokuren() {
    // init base config
    base_config.interval_ms = 60000;
    base_config.tick_size = 1.0;
  }

  // async grid search
  void RunGridSearch(const std::string &filepath) {
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
    unsigned int max_cores = std::thread::hardware_concurrency();
    if (max_cores == 0) max_cores = 4; // fallback
    std::cout << "using " << max_cores << " parallel threads...\n";
    std::cout << "start parameter scan (multithreaded)...\n";

    // count iterations for progress bar
    int total_iterations = 0;
    for (double delta = 0.0; delta <= 0.5; delta += 0.05) {
      for (double sl_pct = 0.002; sl_pct <= 0.010; sl_pct += 0.002) {
        for (double tp_pct = 0.005; tp_pct <= 0.020; tp_pct += 0.005) {
          total_iterations++;
        }
      }
    }

    std::atomic<int> current_iteration{0};

    // multithreading loop
    for (double delta = 0.0; delta <= 0.5; delta += 0.05) {
      for (double sl_pct = 0.002; sl_pct <= 0.010; sl_pct += 0.002) {
        for (double tp_pct = 0.005; tp_pct <= 0.020; tp_pct += 0.005) {

          futures.push_back(std::async(std::launch::async, [=, &all_trades, &results, &results_mutex, &console_mutex, &current_iteration, this]() {
            
            // init modules per thread
            Aggregator aggregator(base_config.interval_ms, base_config.tick_size);
            Aggregator htf_aggregator(15 * 60 * 1000, base_config.tick_size);
            PaperTrader ptrader(sl_pct, tp_pct);
            PositionManager pos_manager(&ptrader);

            DeltaAbsorptionTrigger delta_trigger(delta);
            MinVolumeFilter vol_filter(1.0);
            MacroTrendFilter htf_filter;

            PipelineStrategy pipeline(&delta_trigger);
            pipeline.AddFilter(&vol_filter);
            pipeline.AddFilter(&htf_filter);
            AlphaEngine alpha(pipeline);

            PositionSizer sizer(ptrader, 0.01, 10.0);

            SinglePositionLock pos_lock(ptrader);
            MaxDrawdownLock dd_lock(ptrader, 200.0);
            MaxLeverageLock lev_lock(ptrader, 10.0);

            RiskManager risk_manager;
            risk_manager.AddModule(&pos_lock);
            risk_manager.AddModule(&lev_lock);

            Bar live_bar;
            Bar htf_bar;

            // tick loop
            for (const auto &trade : all_trades) {
              pos_manager.Update(trade.price);
              ptrader.CheckRisk(trade.price);

              bool candle_finished = aggregator.ProcessTrade(live_bar, trade);
              htf_aggregator.ProcessTrade(htf_bar, trade);
              MarketContext context{aggregator.GetHistory(), live_bar, htf_bar};

              TradeSignal raw_signal = alpha.Evaluate(context, candle_finished);

              if (raw_signal.direction != SignalDirection::NONE) {
                raw_signal.entry_price = trade.price;

                if (raw_signal.direction == SignalDirection::BUY) {
                  raw_signal.stop_loss = trade.price * (1.0 - sl_pct);
                  raw_signal.take_profit = trade.price * (1.0 + tp_pct);
                } else {
                  raw_signal.stop_loss = trade.price * (1.0 + sl_pct);
                  raw_signal.take_profit = trade.price * (1.0 - tp_pct);
                }
              }

              TradeSignal sized_signal = sizer.CalculateSize(raw_signal);
              TradeSignal final_signal = risk_manager.Evaluate(sized_signal);

              if (final_signal.direction != SignalDirection::NONE) {
                ptrader.ProcessSignal(final_signal, trade.price);
              }
            }

            aggregator.FlushLastCandle(live_bar);
            ptrader.CloseOpenPositionAtEnd(live_bar.close);

            // save results thread-safe
            if (ptrader.GetTotalTrades() > 0) {
              std::lock_guard<std::mutex> lock(results_mutex);
              results.push_back({delta, sl_pct, tp_pct, ptrader.GetNetProfit(),
                                 ptrader.GetTotalTrades(), ptrader.GetWinrate()});
            }

            // draw progress bar thread-safe
            int done = ++current_iteration;
            {
              std::lock_guard<std::mutex> lock(console_mutex);
              int bar_width = 50;
              float progress = static_cast<float>(done) / total_iterations;
              int pos = static_cast<int>(bar_width * progress);

              std::cout << "\r"; // springt zum anfang der zeile
              for (int i = 0; i < bar_width; ++i) {
                if (i < pos) {
                  // reines rot (rgb: 255, 0, 0)
                  std::cout << "\033[38;2;255;0;0m█";
                } else {
                  // unfertiger bereich (dunkelgrau)
                  std::cout << "\033[38;2;40;40;40m█";
                }
              }
              // farbe zurücksetzen (\033[0m) und prozentzahl drucken
              std::cout << "\033[0m " << static_cast<int>(progress * 100.0) << " %" << std::flush;
            }
          }));

          // throttle threads to prevent oom
          if (futures.size() >= max_cores) {
            for (auto &f : futures) {
              f.get();
            }
            futures.clear();
          }
        }
      }
    }

    // wait for remaining threads
    for (auto &f : futures) {
      f.get();
    }
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
      std::printf("%.1f\t%.1f%%\t%.1f%%\t%d\t%.1f%%\t%.2f\n", r.delta,
                  (r.sl_pct * 100.0), (r.tp_pct * 100.0), r.trades, r.winrate,
                  r.net_profit);
    }
  }
};
