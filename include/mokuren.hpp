#pragma once
#include "config_parser.hpp"
#include "csv_parser.hpp"
#include "factory.hpp"
#include "market_context.hpp"
#include "ui_footprint.hpp"
#include "replay_manager.hpp"
#include "engine_state.hpp"

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
#include <random>
#include <cmath>
#include <ctime>
#include <iomanip>
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
  std::vector<WFAOOSConfig> wfa_configs;
  
  double tp_pct = 0.0;
  double be_pct = 0.0;
  double sl_pct = 0.0;

  double sharpe = 0.0;
  double sortino = 0.0;
  double mc_drawdown = 0.0;
};

struct WFAWindow {
  int64_t in_sample_start;
  int64_t in_sample_end;
  int64_t out_of_sample_start;
  int64_t out_of_sample_end;
};

struct AdvancedMetrics {
    double sharpe = 0.0;
    double sortino = 0.0;
    double mc_dd = 0.0;
};

AdvancedMetrics CalculateAdvancedMetrics(const std::vector<TradeRecord>& trades) {
    AdvancedMetrics m;
    if (trades.size() < 2) return m;

    double sum_return = 0.0;
    std::vector<double> returns;
    for (const auto& t : trades) {
        returns.push_back(t.net_profit);
        sum_return += t.net_profit;
    }
    double mean = sum_return / returns.size();

    double var = 0.0, down_var = 0.0;
    for (double r : returns) {
        var += (r - mean) * (r - mean);
        if (r < 0) down_var += r * r;
    }
    var /= returns.size();
    down_var /= returns.size();

    double std_dev = std::sqrt(var);
    double down_dev = std::sqrt(down_var);

    if (std_dev > 0.0001) m.sharpe = (mean / std_dev) * std::sqrt(trades.size());
    if (down_dev > 0.0001) m.sortino = (mean / down_dev) * std::sqrt(trades.size());

    std::mt19937 rng(42); 
    std::vector<double> sim = returns;
    double worst_dd = 0.0;
    for (int i = 0; i < 1000; i++) {
        std::shuffle(sim.begin(), sim.end(), rng);
        double peak = 10000.0, balance = 10000.0, max_dd = 0.0;
        for (double r : sim) {
            balance += r;
            if (balance > peak) peak = balance;
            double dd = (peak - balance) / peak * 100.0;
            if (dd > max_dd) max_dd = dd;
        }
        if (max_dd > worst_dd) worst_dd = max_dd;
    }
    m.mc_dd = worst_dd;

    return m;
}

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
    sliced.reserve(source_trades.size() / 4); 

    for (const auto& trade : source_trades) {
      if (trade.timestamp >= start_time && trade.timestamp <= end_time) {
        sliced.push_back(trade);
      }
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

      if (win.out_of_sample_end > last_ts) {
        break; 
      }

      windows.push_back(win);
      current_start += step_ms; 
    }
    return windows;
  }

  void ApplyRMultiplesManagement(const nlohmann::json& json_config, double base_sl_pct, EngineInstance& eng) {
      auto* pt = dynamic_cast<PaperTrader*>(eng.ptrader.get());
      if (pt) {
          auto tm = json_config.value("trade_management", nlohmann::json::object());
          auto be = tm.value("break_even", nlohmann::json::object());
          auto tr = tm.value("trailing", nlohmann::json::object());
          auto so = tm.value("scale_out", nlohmann::json::object());

          double be_trig = be.value("enabled", false) ? (base_sl_pct * be.value("trigger_r", 1.5)) : 999.0;
          double be_targ = base_sl_pct * be.value("target_r", 0.25);
          
          bool tr_en = tr.value("enabled", false);
          double tr_trig = base_sl_pct * tr.value("trigger_r", 2.0);
          double tr_dist = base_sl_pct * tr.value("distance_r", 0.5);

          bool so_en = so.value("enabled", false);
          double so_trig = base_sl_pct * so.value("trigger_r", 2.0);
          double so_frac = so.value("fraction", 0.5);

          pt->ApplyManagementConfig(be_trig, be_targ, tr_en, tr_trig, tr_dist, so_en, so_trig, so_frac);
      }
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
      on_status("ERR: FAILED TO LOAD CSV DATA");
      return;
    }

    unsigned int max_cores = base_json["environment"].value("max_cores", 0);
    if (max_cores == 0) max_cores = std::thread::hardware_concurrency();
    
    on_status("GRID SCAN ACTIVE (" + std::to_string(max_cores) + " THREADS)");

    int total_iterations = runs.size();
    std::atomic<size_t> task_index{0};
    std::atomic<int> current_iteration{0};

    // --- workspace setup & provenance ---
    fs::create_directories("runs");
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::ostringstream oss;
    oss << "runs/scan_" << std::put_time(&tm, "%Y%m%d_%H%M%S");
    std::string run_dir = oss.str();
    fs::create_directories(run_dir);

    std::ofstream cfg_out(run_dir + "/used_config.json");
    if (cfg_out.is_open()) {
        cfg_out << base_json.dump(4);
        cfg_out.close();
    }

    std::mutex csv_mutex;
    std::ofstream csv_file(run_dir + "/results.csv");
    bool header_written = false;
    // ------------------------------------

    std::vector<std::thread> workers;
    for (unsigned int i = 0; i < max_cores; ++i) {
      workers.emplace_back([&, this]() {
        while (true) {
          size_t idx = task_index.fetch_add(1);
          if (idx >= runs.size()) break; 
          
          const auto& run = runs[idx];
          AppConfig cfg = AppConfig::Load(run.full_json);
          
          cfg.sl_pct = run.full_json.value("risk", nlohmann::json::object()).value("sl_price_pct", run.full_json.value("risk", nlohmann::json::object()).value("sl_pct", 0.004));
          cfg.tp_pct = cfg.sl_pct * run.full_json.value("risk", nlohmann::json::object()).value("tp_r", 3.0);
          
          EngineInstance eng = StrategyFactory::Build(cfg);
          ApplyRMultiplesManagement(run.full_json, cfg.sl_pct, eng);

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
            
            // pure pnl based performance classification
            int wins = 0, be = 0, losses = 0;
            for (const auto& t : res.trade_log) {
                if (t.net_profit > 0.0) wins++;
                else if (t.net_profit < 0.0) losses++;
                else be++;
            }
            double total = res.trade_log.size();
            res.tp_pct = total > 0 ? (wins / total) * 100.0 : 0.0;
            res.be_pct = total > 0 ? (be / total) * 100.0 : 0.0;
            res.sl_pct = total > 0 ? (losses / total) * 100.0 : 0.0;

            AdvancedMetrics am = CalculateAdvancedMetrics(res.trade_log);
            res.sharpe = am.sharpe;
            res.sortino = am.sortino;
            res.mc_drawdown = am.mc_dd;
            
            on_result(res);

            // thread-safe csv write
            {
              std::lock_guard<std::mutex> lock(csv_mutex);
              if (csv_file.is_open()) {
                if (!header_written) {
                  for (const auto& [key, val] : run.grid_values) {
                    csv_file << key << ",";
                  }
                  csv_file << "NetProfit,MaxDrawdown,Trades,WinRate,Sharpe\n";
                  header_written = true;
                }
                
                for (const auto& [key, val] : run.grid_values) {
                  csv_file << val << ",";
                }
                csv_file << res.net_profit << ","
                         << res.max_drawdown << ","
                         << res.trades << ","
                         << res.winrate << ","
                         << res.sharpe << "\n";
                csv_file.flush();
              }
            }
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
              std::function<void(const std::string&)> on_status,
              std::function<void(int, int)> on_progress,
              std::function<void(const TestResult&)> on_result) {

    on_status("LOADING TICKS FOR WFA...");
    std::vector<TradeEvent> all_trades = LoadAllTrades(filepath);
    if (all_trades.empty()) {
      on_status("ERR: FAILED TO LOAD CSV DATA");
      return;
    }

    int64_t first_ts = all_trades.front().timestamp;
    int64_t last_ts = all_trades.back().timestamp;

    nlohmann::json base_json;
    try {
        std::ifstream file(config_path);
        if (file.is_open()) base_json = nlohmann::json::parse(file);
    } catch (...) {}

    int64_t in_days = base_json.value("backtest", nlohmann::json::object()).value("wfa_in_sample_days", 14);
    int64_t out_days = base_json.value("backtest", nlohmann::json::object()).value("wfa_out_of_sample_days", 7);
    int64_t step_days = base_json.value("backtest", nlohmann::json::object()).value("wfa_step_days", 7);
    
    unsigned int max_cores = base_json.value("environment", nlohmann::json::object()).value("max_cores", 0);
    if (max_cores == 0) max_cores = std::thread::hardware_concurrency();
    if (max_cores == 0) max_cores = 4;

    int64_t day_ms = 86400000LL; 
    int64_t in_sample_ms = in_days * day_ms;    
    int64_t out_of_sample_ms = out_days * day_ms; 
    int64_t step_ms = step_days * day_ms;          

    std::vector<WFAWindow> windows = GenerateWFAWindows(first_ts, last_ts, in_sample_ms, out_of_sample_ms, step_ms);
    
    if (windows.empty()) {
        std::string err = "ERR: CSV TOO SHORT (NEEDS " + std::to_string(in_days + out_days) + " DAYS)";
        on_status(err);
        return;
    }
    
    on_status("WFA: GENERATED " + std::to_string(windows.size()) + " ROLLING WINDOWS");

    std::vector<TradeRecord> global_oos_trades;
    std::vector<WFAOOSConfig> collected_configs;

    for (size_t i = 0; i < windows.size(); ++i) {
      const auto& win = windows[i];
      on_status("WFA WINDOW " + std::to_string(i + 1) + "/" + std::to_string(windows.size()) + " (PARALLEL)");

      std::vector<TradeEvent> in_sample_data = SliceTrades(all_trades, win.in_sample_start, win.in_sample_end);
      std::vector<TradeEvent> out_of_sample_data = SliceTrades(all_trades, win.out_of_sample_start, win.out_of_sample_end);

      if (base_json.empty()) continue; 
      std::vector<RunConfig> runs = GridScanner::GenerateGrid(base_json);

      double best_in_sample_profit = -999999.0;
      RunConfig best_config;
      if (!runs.empty()) best_config = runs[0];

      std::mutex best_mutex;
      std::atomic<size_t> task_index{0};
      std::vector<std::thread> workers;

      for (unsigned int c = 0; c < max_cores; ++c) {
        workers.emplace_back([&]() {
          while (true) {
            size_t idx = task_index.fetch_add(1);
            if (idx >= runs.size()) break;

            const auto& run = runs[idx];
            AppConfig cfg = AppConfig::Load(run.full_json);
            
            cfg.sl_pct = run.full_json.value("risk", nlohmann::json::object()).value("sl_price_pct", run.full_json.value("risk", nlohmann::json::object()).value("sl_pct", 0.004));
            cfg.tp_pct = cfg.sl_pct * run.full_json.value("risk", nlohmann::json::object()).value("tp_r", 3.0);
            
            EngineInstance eng = StrategyFactory::Build(cfg);
            ApplyRMultiplesManagement(run.full_json, cfg.sl_pct, eng);

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
            
            std::lock_guard<std::mutex> lock(best_mutex);
            if (profit > best_in_sample_profit) {
              best_in_sample_profit = profit;
              best_config = run;
            }
          }
        });
      }

      for (auto& w : workers) {
        if (w.joinable()) w.join();
      }

      AppConfig oos_cfg = AppConfig::Load(best_config.full_json);
      oos_cfg.sl_pct = best_config.full_json.value("risk", nlohmann::json::object()).value("sl_price_pct", best_config.full_json.value("risk", nlohmann::json::object()).value("sl_pct", 0.004));
      oos_cfg.tp_pct = oos_cfg.sl_pct * best_config.full_json.value("risk", nlohmann::json::object()).value("tp_r", 3.0);

      EngineInstance oos_eng = StrategyFactory::Build(oos_cfg);
      ApplyRMultiplesManagement(best_config.full_json, oos_cfg.sl_pct, oos_eng);

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

      auto* oos_pt = dynamic_cast<PaperTrader*>(oos_eng.ptrader.get());
      const auto& window_trades = oos_pt->GetTradeHistory();
      global_oos_trades.insert(global_oos_trades.end(), window_trades.begin(), window_trades.end());
      
      collected_configs.push_back({win.out_of_sample_start, win.out_of_sample_end, best_config.full_json});

      on_progress(i + 1, windows.size());
    } 

    if (!global_oos_trades.empty()) {
      TestResult res;
      res.parameters["WFA_Windows_Done"] = windows.size(); 
      
      nlohmann::json final_cfg = collected_configs.back().config;
      final_cfg["backtest"]["wfa_mode"] = true; 
      res.full_config = final_cfg; 
      
      res.trades = global_oos_trades.size();
      res.trade_log = global_oos_trades;
      res.wfa_configs = collected_configs; 
      
      double current_balance = 10000.0, peak_balance = 10000.0;
      double max_dd = 0.0, net_profit = 0.0;
      int wins = 0, be = 0, losses = 0;

      for (const auto& t : global_oos_trades) {
        net_profit += t.net_profit;
        current_balance += t.net_profit;
        if (current_balance > peak_balance) peak_balance = current_balance;
        double dd = (peak_balance - current_balance) / peak_balance * 100.0;
        if (dd > max_dd) max_dd = dd;

        // pure pnl based performance classification
        if (t.net_profit > 0.0) wins++;
        else if (t.net_profit < 0.0) losses++;
        else be++;
      }

      res.net_profit = net_profit;
      res.max_drawdown = max_dd;
      res.winrate = res.trades > 0 ? (double)wins / res.trades * 100.0 : 0.0;
      res.tp_pct = res.trades > 0 ? (double)wins / res.trades * 100.0 : 0.0;
      res.be_pct = res.trades > 0 ? (double)be / res.trades * 100.0 : 0.0;
      res.sl_pct = res.trades > 0 ? (double)losses / res.trades * 100.0 : 0.0;

      AdvancedMetrics am = CalculateAdvancedMetrics(global_oos_trades);
      res.sharpe = am.sharpe;
      res.sortino = am.sortino;
      res.mc_drawdown = am.mc_dd;

      on_result(res);
    }

    on_status("WFA COMPLETE");
  }

  ReplayResult ReplaySingleRun(const std::string &filepath, const nlohmann::json& winning_config) {
      ReplayResult result;

      std::vector<TradeEvent> all_trades = LoadAllTrades(filepath);
      if (all_trades.empty()) return result;

      AppConfig cfg = AppConfig::Load(winning_config);
      cfg.sl_pct = winning_config.value("risk", nlohmann::json::object()).value("sl_price_pct", winning_config.value("risk", nlohmann::json::object()).value("sl_pct", 0.004));
      cfg.tp_pct = cfg.sl_pct * winning_config.value("risk", nlohmann::json::object()).value("tp_r", 3.0);

      EngineInstance eng = StrategyFactory::Build(cfg);
      ApplyRMultiplesManagement(winning_config, cfg.sl_pct, eng);

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
          ti.exit_price = t.exit_price;
          ti.pnl = t.net_profit;
          ti.exit_reason = t.exit_reason; 
          ti.stop_history = t.stop_events; // pass trailing history 
          
          auto it = std::lower_bound(result.history_1m.begin(), result.history_1m.end(), t.entry_time, 
              [](const Bar& b, int64_t time) { return b.timestamp_start < time; });
              
          if (it != result.history_1m.end()) {
              ti.candle_idx = std::distance(result.history_1m.begin(), it);
              if (ti.candle_idx > 0 && it->timestamp_start > t.entry_time) ti.candle_idx--; 
          } else {
              ti.candle_idx = result.history_1m.empty() ? 0 : result.history_1m.size() - 1;
          }
          
          auto exit_it = std::lower_bound(result.history_1m.begin(), result.history_1m.end(), t.exit_time, 
              [](const Bar& b, int64_t time) { return b.timestamp_start < time; });
              
          if (exit_it != result.history_1m.end()) {
              ti.exit_candle_idx = std::distance(result.history_1m.begin(), exit_it);
              if (ti.exit_candle_idx > 0 && exit_it->timestamp_start > t.exit_time) ti.exit_candle_idx--; 
          } else {
              ti.exit_candle_idx = result.history_1m.empty() ? 0 : result.history_1m.size() - 1;
          }

          // map physical candle indices for trailing history rendering
          for (auto& se : ti.stop_history) {
              auto sit = std::lower_bound(result.history_1m.begin(), result.history_1m.end(), se.timestamp, 
                  [](const Bar& b, int64_t time) { return b.timestamp_start < time; });
              if (sit != result.history_1m.end()) {
                  se.candle_idx = std::distance(result.history_1m.begin(), sit);
                  if (se.candle_idx > 0 && sit->timestamp_start > se.timestamp) se.candle_idx--;
              } else {
                  se.candle_idx = result.history_1m.empty() ? 0 : result.history_1m.size() - 1;
              }
          }

          result.trades.push_back(ti);
      }
      
      for (auto& ti : result.trades) {
          if (result.history_1m.empty()) {
              ti.candle_idx = 0;
              ti.exit_candle_idx = 0;
          } else {
              size_t max_idx = result.history_1m.size() - 1;
              if (ti.candle_idx > max_idx) ti.candle_idx = max_idx;
              if (ti.exit_candle_idx > max_idx) ti.exit_candle_idx = max_idx;
              if (ti.exit_candle_idx < ti.candle_idx) ti.exit_candle_idx = ti.candle_idx;
          }
      }
      
      return result;
  }

  ReplayResult ReplayWFA(const std::string &filepath, const std::vector<WFAOOSConfig>& wfa_configs) {
      ReplayResult result;
      if (wfa_configs.empty()) return result;

      std::vector<TradeEvent> all_trades = LoadAllTrades(filepath);
      if (all_trades.empty()) return result;

      for (const auto& wfa_cfg : wfa_configs) {
          AppConfig cfg = AppConfig::Load(wfa_cfg.config);
          cfg.sl_pct = wfa_cfg.config.value("risk", nlohmann::json::object()).value("sl_price_pct", wfa_cfg.config.value("risk", nlohmann::json::object()).value("sl_pct", 0.004));
          cfg.tp_pct = cfg.sl_pct * wfa_cfg.config.value("risk", nlohmann::json::object()).value("tp_r", 3.0);

          EngineInstance eng = StrategyFactory::Build(cfg);
          ApplyRMultiplesManagement(wfa_cfg.config, cfg.sl_pct, eng);

          std::vector<TradeEvent> oos_data = SliceTrades(all_trades, wfa_cfg.oos_start, wfa_cfg.oos_end);
          Bar live_bar, htf_bar;
          
          for (const auto &trade : oos_data) {
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

          const auto& hist_1m = eng.aggregator->GetHistory();
          const auto& hist_15m = eng.htf_aggregator->GetHistory();
          size_t start_idx_1m = result.history_1m.size();
          
          result.history_1m.insert(result.history_1m.end(), hist_1m.begin(), hist_1m.end());
          result.history_15m.insert(result.history_15m.end(), hist_15m.begin(), hist_15m.end());

          const auto& trade_log = eng.ptrader->GetTradeHistory();
          for (const auto& t : trade_log) {
              TradeInfo ti;
              ti.is_long = (t.direction == SignalDirection::BUY); 
              ti.entry_price = t.entry_price;
              ti.exit_price = t.exit_price;
              ti.pnl = t.net_profit;
              ti.exit_reason = t.exit_reason; 
              ti.stop_history = t.stop_events; // pass trailing history
              
              auto it = std::lower_bound(hist_1m.begin(), hist_1m.end(), t.entry_time, 
                  [](const Bar& b, int64_t time) { return b.timestamp_start < time; });
                  
              if (it != hist_1m.end()) {
                  size_t local_idx = std::distance(hist_1m.begin(), it);
                  if (local_idx > 0 && it->timestamp_start > t.entry_time) local_idx--; 
                  ti.candle_idx = start_idx_1m + local_idx;
              } else {
                  ti.candle_idx = start_idx_1m + (hist_1m.empty() ? 0 : hist_1m.size() - 1);
              }

              auto exit_it = std::lower_bound(hist_1m.begin(), hist_1m.end(), t.exit_time, 
                  [](const Bar& b, int64_t time) { return b.timestamp_start < time; });
                  
              if (exit_it != hist_1m.end()) {
                  size_t local_idx = std::distance(hist_1m.begin(), exit_it);
                  if (local_idx > 0 && exit_it->timestamp_start > t.exit_time) local_idx--; 
                  ti.exit_candle_idx = start_idx_1m + local_idx;
              } else {
                  ti.exit_candle_idx = start_idx_1m + (hist_1m.empty() ? 0 : hist_1m.size() - 1);
              }

              // map physical candle indices for trailing history rendering
              for (auto& se : ti.stop_history) {
                  auto sit = std::lower_bound(hist_1m.begin(), hist_1m.end(), se.timestamp, 
                      [](const Bar& b, int64_t time) { return b.timestamp_start < time; });
                  if (sit != hist_1m.end()) {
                      size_t local_idx = std::distance(hist_1m.begin(), sit);
                      if (local_idx > 0 && sit->timestamp_start > se.timestamp) local_idx--;
                      se.candle_idx = start_idx_1m + local_idx;
                  } else {
                      se.candle_idx = start_idx_1m + (hist_1m.empty() ? 0 : hist_1m.size() - 1);
                  }
              }

              result.trades.push_back(ti);
          }
      }

      for (auto& ti : result.trades) {
          if (result.history_1m.empty()) {
              ti.candle_idx = 0;
              ti.exit_candle_idx = 0;
          } else {
              size_t max_idx = result.history_1m.size() - 1;
              if (ti.candle_idx > max_idx) ti.candle_idx = max_idx;
              if (ti.exit_candle_idx > max_idx) ti.exit_candle_idx = max_idx;
              if (ti.exit_candle_idx < ti.candle_idx) ti.exit_candle_idx = ti.candle_idx;
          }
      }

      return result;
  }
};