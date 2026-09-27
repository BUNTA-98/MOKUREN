#pragma once
#include "config_parser.hpp"
#include "csv_parser.hpp"
#include "factory.hpp"
#include "market_context.hpp"
#include "ui_footprint.hpp"

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
  double max_drawdown; // <--- NEU
  int trades;
  double winrate;
  std::vector<TradeRecord> trade_log;
};

struct ReplayResult {
    std::vector<Bar> history_1m;
    std::vector<Bar> history_15m;
    std::vector<TradeInfo> trades; 
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
            res.max_drawdown = eng.ptrader->GetMaxDrawdown(); // <--- NEU
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

  ReplayResult ReplaySingleRun(const std::string &filepath, const nlohmann::json& winning_config) {
      ReplayResult result;

      std::vector<TradeEvent> all_trades = LoadAllTrades(filepath);
      if (all_trades.empty()) return result;

      AppConfig cfg = AppConfig::Load(winning_config);
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

      result.history_1m = eng.aggregator->GetHistory();
      result.history_15m = eng.htf_aggregator->GetHistory();
      
      const auto& trade_log = eng.ptrader->GetTradeHistory();
      for (const auto& t : trade_log) {
          TradeInfo ti;
          ti.is_long = (t.direction == SignalDirection::BUY); 
          ti.entry_price = t.entry_price;
          ti.pnl = t.net_profit;
          
          auto it = std::lower_bound(result.history_1m.begin(), result.history_1m.end(), t.entry_time, 
              [](const Bar& b, int64_t time) { return b.timestamp_start < time; });
              
          if (it != result.history_1m.end()) {
              ti.candle_idx = std::distance(result.history_1m.begin(), it);
              if (ti.candle_idx > 0 && it->timestamp_start > t.entry_time) ti.candle_idx--; 
          } else {
              ti.candle_idx = result.history_1m.empty() ? 0 : result.history_1m.size() - 1;
          }
          result.trades.push_back(ti);
      }
      
      return result;
  }
};