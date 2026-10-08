#include "mokuren.hpp"
#include "data_manager.hpp"
#include "performance_metrics.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

void Mokuren::ApplyRMultiplesManagement(const nlohmann::json& json_config, double base_sl_pct, EngineInstance& eng) {
    auto* pt = dynamic_cast<PaperTrader*>(eng.ptrader.get());
    if (pt) {
        auto tm = json_config.value("trade_management", nlohmann::json::object());
        auto be = tm.value("break_even", nlohmann::json::object());
        auto tr = tm.value("trailing", nlohmann::json::object());
        auto so = tm.value("scale_out", nlohmann::json::object());

        // map r-multiples to abs pct
        double be_trig = be.value("enabled", false) ? (base_sl_pct * be.value("trigger_r", 1.5)) : 999.0;
        double be_targ = base_sl_pct * be.value("target_r", 0.25);
        
        bool tr_en = tr.value("enabled", false);
        double tr_trig = base_sl_pct * tr.value("trigger_r", 2.0);
        double tr_dist = base_sl_pct * tr.value("distance_r", 0.5);

        bool so_en = so.value("enabled", false);
        double so_trig = base_sl_pct * so.value("trigger_r", 2.0);
        double so_frac = so.value("fraction", 0.5);

        // apply to paper trader
        pt->ApplyManagementConfig(be_trig, be_targ, tr_en, tr_trig, tr_dist, so_en, so_trig, so_frac);
    }
}

void Mokuren::RunGridSearch(const std::string &/*ignored*/, const std::string &config_path,
                            std::function<void(const std::string&)> on_status,
                            std::function<void(int, int)> on_progress,
                            std::function<void(const TestResult&)> on_result) {
    std::ifstream file(config_path);
    if (!file.is_open()) {
        on_status("ERROR: CONFIG NOT FOUND");
        return;
    }
    
    // parse base json
    nlohmann::json base_json = nlohmann::json::parse(file);
    
    // gen params
    std::vector<RunConfig> runs = GridScanner::GenerateGrid(base_json);
    
    std::string active_dataset = base_json.value("environment", nlohmann::json::object()).value("dataset", "data");
    on_status("LOADING DATASET: " + active_dataset);
    
    // load full dataset to ram via data manager
    std::vector<TradeEvent> all_trades = DataManager::LoadAllTrades(active_dataset);
    if (all_trades.empty()) {
        on_status("ERR: NO TRADES FOUND IN DATASET");
        return;
    }

    std::vector<L2Snapshot> all_l2 = DataManager::LoadAllL2Snapshots(active_dataset);
    if (all_l2.empty()) {
        on_status("WARN: NO L2 DATA (L2_cache.bin) FOUND");
    } else {
        if (!DataManager::ValidateDataAlignment(all_trades, all_l2, on_status)) return;
    }

    // setup threads
    unsigned int max_cores = base_json["environment"].value("max_cores", 0);
    if (max_cores == 0) max_cores = std::thread::hardware_concurrency();
    
    on_status("GRID SCAN ACTIVE (" + std::to_string(max_cores) + " THREADS)");

    int total_iterations = runs.size();
    std::atomic<size_t> task_index{0};
    std::atomic<int> current_iteration{0};

    // create out dir
    fs::create_directories("runs");
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::ostringstream oss;
    oss << "runs/scan_" << std::put_time(&tm, "%Y%m%d_%H%M%S");
    std::string run_dir = oss.str();
    fs::create_directories(run_dir);

    // save base config
    std::ofstream cfg_out(run_dir + "/used_config.json");
    if (cfg_out.is_open()) {
        cfg_out << base_json.dump(4);
        cfg_out.close();
    }

    // sync csv out
    std::mutex csv_mutex;
    std::ofstream csv_file(run_dir + "/results.csv");
    bool header_written = false;

    // spawn workers
    std::vector<std::thread> workers;
    for (unsigned int i = 0; i < max_cores; ++i) {
        workers.emplace_back([&, this]() {
            while (true) {
                // fetch next job atomic
                size_t idx = task_index.fetch_add(1);
                if (idx >= runs.size()) break; 
                
                const auto& run = runs[idx];
                AppConfig cfg = AppConfig::Load(run.full_json);
                
                // get sl and tp
                cfg.sl_pct = run.full_json.value("risk", nlohmann::json::object()).value("sl_price_pct", run.full_json.value("risk", nlohmann::json::object()).value("sl_pct", 0.004));
                cfg.tp_pct = cfg.sl_pct * run.full_json.value("risk", nlohmann::json::object()).value("tp_r", 3.0);
                
                // init strat mods
                EngineInstance eng = StrategyFactory::Build(cfg);
                ApplyRMultiplesManagement(run.full_json, cfg.sl_pct, eng);

                // main signal exec funnel
                auto execute_signal = [&](TradeSignal raw_sig, double exec_price, int64_t ts) {
                    if (raw_sig.direction == SignalDirection::NONE) return;
                    
                    // set targets
                    raw_sig.entry_price = exec_price;
                    raw_sig.stop_loss = exec_price * (raw_sig.direction == SignalDirection::BUY ? (1.0 - cfg.sl_pct) : (1.0 + cfg.sl_pct));
                    raw_sig.take_profit = exec_price * (raw_sig.direction == SignalDirection::BUY ? (1.0 + cfg.tp_pct) : (1.0 - cfg.tp_pct));
                    
                    // apply pos sizing
                    TradeSignal sized = eng.sizer->CalculateSize(raw_sig);
                    
                    // final risk check
                    TradeSignal final_sig = eng.risk_manager->Evaluate(sized, ts);
                    if (final_sig.direction != SignalDirection::NONE) {
                        eng.ptrader->ProcessSignal(final_sig, exec_price, ts);
                    }
                };

                Bar live_bar;
                Bar htf_bar;
                size_t l2_idx = 0;

                for (const auto &trade : all_trades) {
                    // catch up l2
                    while (l2_idx < all_l2.size() && all_l2[l2_idx].timestamp <= trade.timestamp) {
                        // update l2 state
                        eng.aggregator->ProcessL2(all_l2[l2_idx]);
                        eng.htf_aggregator->ProcessL2(all_l2[l2_idx]);
                        
                        // inject market state
                        MarketContext tick_ctx{
                            eng.aggregator->GetHistory(), live_bar, htf_bar, 
                            eng.aggregator->GetSessionMetrics(), eng.aggregator->GetLatestL2(), eng.aggregator->GetL2History()
                        };

                        // eval tick sigs
                        TradeSignal tick_sig = eng.alpha->Evaluate(tick_ctx, false);
                        
                        if (tick_sig.direction != SignalDirection::NONE) {
                            // cross spread for hft exec
                            double exec_price = (tick_sig.direction == SignalDirection::BUY) 
                                              ? all_l2[l2_idx].best_ask_price 
                                              : all_l2[l2_idx].best_bid_price;
                            
                            // fallback if no price
                            if (exec_price <= 0.0) exec_price = live_bar.close > 0.0 ? live_bar.close : trade.price;
                            
                            execute_signal(tick_sig, exec_price, all_l2[l2_idx].timestamp);
                        }
                        
                        l2_idx++;
                    }

                    // process trade event (passing ts to pos manager)
                    eng.pos_manager->Update(trade.price, trade.timestamp);
                    eng.ptrader->CheckRisk(trade.price, trade.timestamp);

                    // build ohlcv
                    bool candle_finished = eng.aggregator->ProcessTrade(live_bar, trade);
                    eng.htf_aggregator->ProcessTrade(htf_bar, trade);
                    
                    MarketContext context{
                        eng.aggregator->GetHistory(), live_bar, htf_bar, 
                        eng.aggregator->GetSessionMetrics(), eng.aggregator->GetLatestL2(), eng.aggregator->GetL2History()
                    };

                    // eval candle sigs
                    TradeSignal candle_sig = eng.alpha->Evaluate(context, candle_finished);
                    execute_signal(candle_sig, trade.price, trade.timestamp);
                }

                // force close at end
                eng.aggregator->FlushLastCandle(live_bar);
                eng.ptrader->CloseOpenPositionAtEnd(live_bar.close, live_bar.timestamp_start);

                // compile metrics
                if (eng.ptrader->GetTotalTrades() > 0) {
                    TestResult res;
                    res.parameters = run.grid_values;
                    res.full_config = run.full_json; 
                    res.net_profit = eng.ptrader->GetNetProfit();
                    res.max_drawdown = eng.ptrader->GetMaxDrawdown(); 
                    res.trades = eng.ptrader->GetTotalTrades();
                    res.winrate = eng.ptrader->GetWinrate();
                    res.trade_log = eng.ptrader->GetTradeHistory();
                    
                    // calc outcome pct
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

                    // calc adv metrics
                    AdvancedMetrics am = PerformanceMetrics::CalculateAdvancedMetrics(res.trade_log);
                    res.sharpe = am.sharpe;
                    res.sortino = am.sortino;
                    res.mc_drawdown = am.mc_dd;
                    
                    on_result(res);

                    // safe csv write
                    {
                        std::lock_guard<std::mutex> lock(csv_mutex);
                        if (csv_file.is_open()) {
                            // write dyn headers once
                            if (!header_written) {
                                for (const auto& [key, val] : run.grid_values) {
                                    csv_file << key << ",";
                                }
                                csv_file << "NetProfit,MaxDrawdown,Trades,WinRate,Sharpe\n";
                                header_written = true;
                            }
                            
                            // write row
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

    // wait for workers
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }
    
    on_status("SCAN COMPLETE");
}

void Mokuren::RunWFA(const std::string &/*ignored*/, const std::string &config_path,
                     std::function<void(const std::string&)> on_status,
                     std::function<void(int, int)> on_progress,
                     std::function<void(const TestResult&)> on_result) {
    nlohmann::json base_json;
    try {
        std::ifstream file(config_path);
        if (file.is_open()) base_json = nlohmann::json::parse(file);
    } catch (...) {}

    std::string active_dataset = base_json.value("environment", nlohmann::json::object()).value("dataset", "data");
    on_status("LOADING DATASET FOR WFA: " + active_dataset);
    
    std::vector<TradeEvent> all_trades = DataManager::LoadAllTrades(active_dataset);
    if (all_trades.empty()) {
        on_status("ERR: NO TRADES FOUND IN DATASET");
        return;
    }

    std::vector<L2Snapshot> all_l2 = DataManager::LoadAllL2Snapshots(active_dataset);
    if (!DataManager::ValidateDataAlignment(all_trades, all_l2, on_status)) return;

    int64_t first_ts = all_trades.front().timestamp;
    int64_t last_ts = all_trades.back().timestamp;

    // get wfa params
    int64_t in_days = base_json.value("backtest", nlohmann::json::object()).value("wfa_in_sample_days", 14);
    int64_t out_days = base_json.value("backtest", nlohmann::json::object()).value("wfa_out_of_sample_days", 7);
    int64_t step_days = base_json.value("backtest", nlohmann::json::object()).value("wfa_step_days", 7);
    
    unsigned int max_cores = base_json.value("environment", nlohmann::json::object()).value("max_cores", 0);
    if (max_cores == 0) max_cores = std::thread::hardware_concurrency();
    if (max_cores == 0) max_cores = 4;

    // conv days to ms
    int64_t day_ms = 86400000LL; 
    int64_t in_sample_ms = in_days * day_ms;    
    int64_t out_of_sample_ms = out_days * day_ms; 
    int64_t step_ms = step_days * day_ms;          

    std::vector<WFAWindow> windows = DataManager::GenerateWFAWindows(first_ts, last_ts, in_sample_ms, out_of_sample_ms, step_ms);
    if (windows.empty()) {
        on_status("ERR: CSV TOO SHORT (NEEDS " + std::to_string(in_days + out_days) + " DAYS)");
        return;
    }
    on_status("WFA: GENERATED " + std::to_string(windows.size()) + " ROLLING WINDOWS");

    std::vector<TradeRecord> global_oos_trades;
    std::vector<WFAOOSConfig> collected_configs;

    // run wfa windows seq
    for (size_t i = 0; i < windows.size(); ++i) {
        const auto& win = windows[i];
        on_status("WFA WINDOW " + std::to_string(i + 1) + "/" + std::to_string(windows.size()) + " (PARALLEL)");

        // isolate window data via data mgr
        std::vector<TradeEvent> in_sample_data = DataManager::SliceTrades(all_trades, win.in_sample_start, win.in_sample_end);
        std::vector<TradeEvent> out_of_sample_data = DataManager::SliceTrades(all_trades, win.out_of_sample_start, win.out_of_sample_end);
        std::vector<L2Snapshot> in_sample_l2 = DataManager::SliceL2(all_l2, win.in_sample_start, win.in_sample_end);
        std::vector<L2Snapshot> out_of_sample_l2 = DataManager::SliceL2(all_l2, win.out_of_sample_start, win.out_of_sample_end);

        if (base_json.empty()) continue; 
        std::vector<RunConfig> runs = GridScanner::GenerateGrid(base_json);

        double best_in_sample_profit = -999999.0;
        RunConfig best_config;
        if (!runs.empty()) best_config = runs[0];

        std::mutex best_mutex;
        std::atomic<size_t> task_index{0};
        std::vector<std::thread> workers;

        // exec in-sample parallel
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

                    // exec funnel
                    auto execute_signal = [&](TradeSignal raw_sig, double exec_price, int64_t ts) {
                        if (raw_sig.direction == SignalDirection::NONE) return;
                        raw_sig.entry_price = exec_price;
                        raw_sig.stop_loss = exec_price * (raw_sig.direction == SignalDirection::BUY ? (1.0 - cfg.sl_pct) : (1.0 + cfg.sl_pct));
                        raw_sig.take_profit = exec_price * (raw_sig.direction == SignalDirection::BUY ? (1.0 + cfg.tp_pct) : (1.0 - cfg.tp_pct));
                        
                        TradeSignal sized = eng.sizer->CalculateSize(raw_sig);
                        TradeSignal final_sig = eng.risk_manager->Evaluate(sized, ts);
                        if (final_sig.direction != SignalDirection::NONE) {
                            eng.ptrader->ProcessSignal(final_sig, exec_price, ts);
                        }
                    };

                    Bar live_bar, htf_bar;
                    size_t l2_idx = 0;

                    for (const auto &trade : in_sample_data) {
                        // catch up l2
                        while (l2_idx < in_sample_l2.size() && in_sample_l2[l2_idx].timestamp <= trade.timestamp) {
                            eng.aggregator->ProcessL2(in_sample_l2[l2_idx]);
                            eng.htf_aggregator->ProcessL2(in_sample_l2[l2_idx]);
                            
                            MarketContext tick_ctx{
                                eng.aggregator->GetHistory(), live_bar, htf_bar, 
                                eng.aggregator->GetSessionMetrics(), eng.aggregator->GetLatestL2(), eng.aggregator->GetL2History()
                            };

                            TradeSignal tick_sig = eng.alpha->Evaluate(tick_ctx, false);
                            if (tick_sig.direction != SignalDirection::NONE) {
                                double exec_price = (tick_sig.direction == SignalDirection::BUY) 
                                                  ? in_sample_l2[l2_idx].best_ask_price 
                                                  : in_sample_l2[l2_idx].best_bid_price;
                                
                                if (exec_price <= 0.0) exec_price = live_bar.close > 0.0 ? live_bar.close : trade.price;
                                execute_signal(tick_sig, exec_price, in_sample_l2[l2_idx].timestamp);
                            }
                            l2_idx++;
                        }

                        // update pos risk (passing ts to pos manager)
                        eng.pos_manager->Update(trade.price, trade.timestamp);
                        eng.ptrader->CheckRisk(trade.price, trade.timestamp);
                        
                        // build ohlcv
                        bool candle_finished = eng.aggregator->ProcessTrade(live_bar, trade);
                        eng.htf_aggregator->ProcessTrade(htf_bar, trade);
                        
                        MarketContext context{
                            eng.aggregator->GetHistory(), live_bar, htf_bar, 
                            eng.aggregator->GetSessionMetrics(), eng.aggregator->GetLatestL2(), eng.aggregator->GetL2History()
                        };

                        TradeSignal candle_sig = eng.alpha->Evaluate(context, candle_finished);
                        execute_signal(candle_sig, trade.price, trade.timestamp);
                    }
                    
                    eng.aggregator->FlushLastCandle(live_bar);
                    eng.ptrader->CloseOpenPositionAtEnd(live_bar.close, live_bar.timestamp_start);

                    double profit = eng.ptrader->GetNetProfit();
                    
                    // track best is run
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

        // exec winning config in oos
        AppConfig oos_cfg = AppConfig::Load(best_config.full_json);
        oos_cfg.sl_pct = best_config.full_json.value("risk", nlohmann::json::object()).value("sl_price_pct", best_config.full_json.value("risk", nlohmann::json::object()).value("sl_pct", 0.004));
        oos_cfg.tp_pct = oos_cfg.sl_pct * best_config.full_json.value("risk", nlohmann::json::object()).value("tp_r", 3.0);

        EngineInstance oos_eng = StrategyFactory::Build(oos_cfg);
        ApplyRMultiplesManagement(best_config.full_json, oos_cfg.sl_pct, oos_eng);

        auto execute_oos_signal = [&](TradeSignal raw_sig, double exec_price, int64_t ts) {
            if (raw_sig.direction == SignalDirection::NONE) return;
            raw_sig.entry_price = exec_price;
            raw_sig.stop_loss = exec_price * (raw_sig.direction == SignalDirection::BUY ? (1.0 - oos_cfg.sl_pct) : (1.0 + oos_cfg.sl_pct));
            raw_sig.take_profit = exec_price * (raw_sig.direction == SignalDirection::BUY ? (1.0 + oos_cfg.tp_pct) : (1.0 - oos_cfg.tp_pct));
            TradeSignal sized = oos_eng.sizer->CalculateSize(raw_sig);
            TradeSignal final_sig = oos_eng.risk_manager->Evaluate(sized, ts);
            if (final_sig.direction != SignalDirection::NONE) {
                oos_eng.ptrader->ProcessSignal(final_sig, exec_price, ts);
            }
        };

        Bar oos_live, oos_htf;
        size_t l2_oos_idx = 0;

        for (const auto &trade : out_of_sample_data) {
            // proc oos l2
            while (l2_oos_idx < out_of_sample_l2.size() && out_of_sample_l2[l2_oos_idx].timestamp <= trade.timestamp) {
                oos_eng.aggregator->ProcessL2(out_of_sample_l2[l2_oos_idx]);
                oos_eng.htf_aggregator->ProcessL2(out_of_sample_l2[l2_oos_idx]);
                
                MarketContext tick_ctx{
                    oos_eng.aggregator->GetHistory(), oos_live, oos_htf, 
                    oos_eng.aggregator->GetSessionMetrics(), oos_eng.aggregator->GetLatestL2(), oos_eng.aggregator->GetL2History()
                };

                TradeSignal tick_sig = oos_eng.alpha->Evaluate(tick_ctx, false);
                if (tick_sig.direction != SignalDirection::NONE) {
                    double exec_price = (tick_sig.direction == SignalDirection::BUY) 
                                      ? out_of_sample_l2[l2_oos_idx].best_ask_price 
                                      : out_of_sample_l2[l2_oos_idx].best_bid_price;
                    
                    if (exec_price <= 0.0) exec_price = oos_live.close > 0.0 ? oos_live.close : trade.price;
                    execute_oos_signal(tick_sig, exec_price, out_of_sample_l2[l2_oos_idx].timestamp);
                }
                l2_oos_idx++;
            }

            // proc oos trades (passing ts to pos manager)
            oos_eng.pos_manager->Update(trade.price, trade.timestamp);
            oos_eng.ptrader->CheckRisk(trade.price, trade.timestamp);
            bool candle_finished = oos_eng.aggregator->ProcessTrade(oos_live, trade);
            oos_eng.htf_aggregator->ProcessTrade(oos_htf, trade);
            
            MarketContext context{
                oos_eng.aggregator->GetHistory(), oos_live, oos_htf, 
                oos_eng.aggregator->GetSessionMetrics(), oos_eng.aggregator->GetLatestL2(), oos_eng.aggregator->GetL2History()
            };

            TradeSignal candle_sig = oos_eng.alpha->Evaluate(context, candle_finished);
            execute_oos_signal(candle_sig, trade.price, trade.timestamp);
        }
        
        oos_eng.aggregator->FlushLastCandle(oos_live);
        oos_eng.ptrader->CloseOpenPositionAtEnd(oos_live.close, oos_live.timestamp_start);

        // agg oos trades for global rep
        auto* oos_pt = dynamic_cast<PaperTrader*>(oos_eng.ptrader.get());
        const auto& window_trades = oos_pt->GetTradeHistory();
        global_oos_trades.insert(global_oos_trades.end(), window_trades.begin(), window_trades.end());
        
        // save config for ui replay
        collected_configs.push_back({win.out_of_sample_start, win.out_of_sample_end, best_config.full_json});
        on_progress(i + 1, windows.size());
    } 

    // comp agg wfa res
    if (!global_oos_trades.empty()) {
        TestResult res;
        res.parameters["WFA_Windows_Done"] = windows.size(); 
        
        nlohmann::json final_cfg = collected_configs.back().config;
        final_cfg["backtest"]["wfa_mode"] = true; 
        res.full_config = final_cfg; 
        
        res.trades = global_oos_trades.size();
        res.trade_log = global_oos_trades;
        res.wfa_configs = collected_configs; 
        
        double current_balance = 10000.0, peak_balance = 10000.0, max_dd = 0.0, net_profit = 0.0;
        int wins = 0, be = 0, losses = 0;

        for (const auto& t : global_oos_trades) {
            net_profit += t.net_profit;
            current_balance += t.net_profit;
            if (current_balance > peak_balance) peak_balance = current_balance;
            double dd = (peak_balance - current_balance) / peak_balance * 100.0;
            if (dd > max_dd) max_dd = dd;

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

        AdvancedMetrics am = PerformanceMetrics::CalculateAdvancedMetrics(global_oos_trades);
        res.sharpe = am.sharpe;
        res.sortino = am.sortino;
        res.mc_drawdown = am.mc_dd;

        on_result(res);
    }

    on_status("WFA COMPLETE");
}

ReplayResult Mokuren::ReplaySingleRun(const std::string &/*ignored*/, const nlohmann::json& winning_config) {
    ReplayResult result;

    std::string active_dataset = winning_config.value("environment", nlohmann::json::object()).value("dataset", "data");
    std::vector<TradeEvent> all_trades = DataManager::LoadAllTrades(active_dataset);
    if (all_trades.empty()) return result;

    std::vector<L2Snapshot> all_l2 = DataManager::LoadAllL2Snapshots(active_dataset);

    AppConfig cfg = AppConfig::Load(winning_config);
    cfg.sl_pct = winning_config.value("risk", nlohmann::json::object()).value("sl_price_pct", winning_config.value("risk", nlohmann::json::object()).value("sl_pct", 0.004));
    cfg.tp_pct = cfg.sl_pct * winning_config.value("risk", nlohmann::json::object()).value("tp_r", 3.0);

    EngineInstance eng = StrategyFactory::Build(cfg);
    ApplyRMultiplesManagement(winning_config, cfg.sl_pct, eng);

    auto execute_signal = [&](TradeSignal raw_sig, double exec_price, int64_t ts) {
        if (raw_sig.direction == SignalDirection::NONE) return;
        raw_sig.entry_price = exec_price;
        raw_sig.stop_loss = exec_price * (raw_sig.direction == SignalDirection::BUY ? (1.0 - cfg.sl_pct) : (1.0 + cfg.sl_pct));
        raw_sig.take_profit = exec_price * (raw_sig.direction == SignalDirection::BUY ? (1.0 + cfg.tp_pct) : (1.0 - cfg.tp_pct));
        TradeSignal sized = eng.sizer->CalculateSize(raw_sig);
        TradeSignal final_sig = eng.risk_manager->Evaluate(sized, ts);
        if (final_sig.direction != SignalDirection::NONE) {
            eng.ptrader->ProcessSignal(final_sig, exec_price, ts);
        }
    };

    Bar live_bar, htf_bar;
    size_t l2_idx = 0;

    for (const auto &trade : all_trades) {
        // catch up l2
        while (l2_idx < all_l2.size() && all_l2[l2_idx].timestamp <= trade.timestamp) {
            eng.aggregator->ProcessL2(all_l2[l2_idx]);
            eng.htf_aggregator->ProcessL2(all_l2[l2_idx]);
            
            MarketContext tick_ctx{
                eng.aggregator->GetHistory(), live_bar, htf_bar, 
                eng.aggregator->GetSessionMetrics(), eng.aggregator->GetLatestL2(), eng.aggregator->GetL2History()
            };

            TradeSignal tick_sig = eng.alpha->Evaluate(tick_ctx, false);
            if (tick_sig.direction != SignalDirection::NONE) {
                double exec_price = (tick_sig.direction == SignalDirection::BUY) 
                                  ? all_l2[l2_idx].best_ask_price 
                                  : all_l2[l2_idx].best_bid_price;
                
                if (exec_price <= 0.0) exec_price = live_bar.close > 0.0 ? live_bar.close : trade.price;
                execute_signal(tick_sig, exec_price, all_l2[l2_idx].timestamp);
            }
            l2_idx++;
        }

        // proc trade (passing ts to pos manager)
        eng.pos_manager->Update(trade.price, trade.timestamp);
        eng.ptrader->CheckRisk(trade.price, trade.timestamp);

        bool candle_finished = eng.aggregator->ProcessTrade(live_bar, trade);
        eng.htf_aggregator->ProcessTrade(htf_bar, trade);
        
        MarketContext context{
            eng.aggregator->GetHistory(), live_bar, htf_bar, 
            eng.aggregator->GetSessionMetrics(), eng.aggregator->GetLatestL2(), eng.aggregator->GetL2History()
        };

        TradeSignal candle_sig = eng.alpha->Evaluate(context, candle_finished);
        execute_signal(candle_sig, trade.price, trade.timestamp);
    }

    eng.aggregator->FlushLastCandle(live_bar);
    eng.ptrader->CloseOpenPositionAtEnd(live_bar.close, live_bar.timestamp_start);

    // populate ui replay data
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
        ti.stop_history = t.stop_events;
        
        // map entry ts to ohlc
        auto it = std::lower_bound(result.history_1m.begin(), result.history_1m.end(), t.entry_time, 
            [](const Bar& b, int64_t time) { return b.timestamp_start < time; });
        if (it != result.history_1m.end()) {
            ti.candle_idx = std::distance(result.history_1m.begin(), it);
            if (ti.candle_idx > 0 && it->timestamp_start > t.entry_time) ti.candle_idx--; 
        } else {
            ti.candle_idx = result.history_1m.empty() ? 0 : result.history_1m.size() - 1;
        }
        
        // map exit ts to ohlc
        auto exit_it = std::lower_bound(result.history_1m.begin(), result.history_1m.end(), t.exit_time, 
            [](const Bar& b, int64_t time) { return b.timestamp_start < time; });
        if (exit_it != result.history_1m.end()) {
            ti.exit_candle_idx = std::distance(result.history_1m.begin(), exit_it);
            if (ti.exit_candle_idx > 0 && exit_it->timestamp_start > t.exit_time) ti.exit_candle_idx--; 
        } else {
            ti.exit_candle_idx = result.history_1m.empty() ? 0 : result.history_1m.size() - 1;
        }

        // map stop events to ohlc
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
    
    // fallbacks for ui
    for (auto& ti : result.trades) {
        if (result.history_1m.empty()) {
            ti.candle_idx = 0; ti.exit_candle_idx = 0;
        } else {
            size_t max_idx = result.history_1m.size() - 1;
            if (ti.candle_idx > max_idx) ti.candle_idx = max_idx;
            if (ti.exit_candle_idx > max_idx) ti.exit_candle_idx = max_idx;
            if (ti.exit_candle_idx < ti.candle_idx) ti.exit_candle_idx = ti.candle_idx;
        }
    }
    
    return result;
}

ReplayResult Mokuren::ReplayWFA(const std::string &/*ignored*/, const std::vector<WFAOOSConfig>& wfa_configs) {
    ReplayResult result;
    if (wfa_configs.empty()) return result;

    std::string active_dataset = wfa_configs[0].config.value("environment", nlohmann::json::object()).value("dataset", "data");
    std::vector<TradeEvent> all_trades = DataManager::LoadAllTrades(active_dataset);
    if (all_trades.empty()) return result;

    std::vector<L2Snapshot> all_l2 = DataManager::LoadAllL2Snapshots(active_dataset);

    for (const auto& wfa_cfg : wfa_configs) {
        AppConfig cfg = AppConfig::Load(wfa_cfg.config);
        cfg.sl_pct = wfa_cfg.config.value("risk", nlohmann::json::object()).value("sl_price_pct", wfa_cfg.config.value("risk", nlohmann::json::object()).value("sl_pct", 0.004));
        cfg.tp_pct = cfg.sl_pct * wfa_cfg.config.value("risk", nlohmann::json::object()).value("tp_r", 3.0);

        EngineInstance eng = StrategyFactory::Build(cfg);
        ApplyRMultiplesManagement(wfa_cfg.config, cfg.sl_pct, eng);

        auto execute_signal = [&](TradeSignal raw_sig, double exec_price, int64_t ts) {
            if (raw_sig.direction == SignalDirection::NONE) return;
            raw_sig.entry_price = exec_price;
            raw_sig.stop_loss = exec_price * (raw_sig.direction == SignalDirection::BUY ? (1.0 - cfg.sl_pct) : (1.0 + cfg.sl_pct));
            raw_sig.take_profit = exec_price * (raw_sig.direction == SignalDirection::BUY ? (1.0 + cfg.tp_pct) : (1.0 - cfg.tp_pct));
            TradeSignal sized = eng.sizer->CalculateSize(raw_sig);
            TradeSignal final_sig = eng.risk_manager->Evaluate(sized, ts);
            if (final_sig.direction != SignalDirection::NONE) {
                eng.ptrader->ProcessSignal(final_sig, exec_price, ts);
            }
        };

        std::vector<TradeEvent> oos_data = DataManager::SliceTrades(all_trades, wfa_cfg.oos_start, wfa_cfg.oos_end);
        std::vector<L2Snapshot> oos_l2 = DataManager::SliceL2(all_l2, wfa_cfg.oos_start, wfa_cfg.oos_end);
        
        Bar live_bar, htf_bar;
        size_t l2_idx = 0;
        
        for (const auto &trade : oos_data) {
            // catch up oos l2
            while (l2_idx < oos_l2.size() && oos_l2[l2_idx].timestamp <= trade.timestamp) {
                eng.aggregator->ProcessL2(oos_l2[l2_idx]);
                eng.htf_aggregator->ProcessL2(oos_l2[l2_idx]);
                
                MarketContext tick_ctx{
                    eng.aggregator->GetHistory(), live_bar, htf_bar, 
                    eng.aggregator->GetSessionMetrics(), eng.aggregator->GetLatestL2(), eng.aggregator->GetL2History()
                };

                TradeSignal tick_sig = eng.alpha->Evaluate(tick_ctx, false);
                if (tick_sig.direction != SignalDirection::NONE) {
                    double exec_price = (tick_sig.direction == SignalDirection::BUY) 
                                      ? oos_l2[l2_idx].best_ask_price 
                                      : oos_l2[l2_idx].best_bid_price;
                    
                    if (exec_price <= 0.0) exec_price = live_bar.close > 0.0 ? live_bar.close : trade.price;
                    execute_signal(tick_sig, exec_price, oos_l2[l2_idx].timestamp);
                }
                l2_idx++;
            }

            // proc trade (passing ts to pos manager)
            eng.pos_manager->Update(trade.price, trade.timestamp);
            eng.ptrader->CheckRisk(trade.price, trade.timestamp);
            bool candle_finished = eng.aggregator->ProcessTrade(live_bar, trade);
            eng.htf_aggregator->ProcessTrade(htf_bar, trade);
            
            MarketContext context{
                eng.aggregator->GetHistory(), live_bar, htf_bar, 
                eng.aggregator->GetSessionMetrics(), eng.aggregator->GetLatestL2(), eng.aggregator->GetL2History()
            };

            TradeSignal candle_sig = eng.alpha->Evaluate(context, candle_finished);
            execute_signal(candle_sig, trade.price, trade.timestamp);
        }
        eng.aggregator->FlushLastCandle(live_bar);
        eng.ptrader->CloseOpenPositionAtEnd(live_bar.close, live_bar.timestamp_start);

        // concat hist seq
        const auto& hist_1m = eng.aggregator->GetHistory();
        const auto& hist_15m = eng.htf_aggregator->GetHistory();
        size_t start_idx_1m = result.history_1m.size();
        
        result.history_1m.insert(result.history_1m.end(), hist_1m.begin(), hist_1m.end());
        result.history_15m.insert(result.history_15m.end(), hist_15m.begin(), hist_15m.end());

        // map ui data for concat
        const auto& trade_log = eng.ptrader->GetTradeHistory();
        for (const auto& t : trade_log) {
            TradeInfo ti;
            ti.is_long = (t.direction == SignalDirection::BUY); 
            ti.entry_price = t.entry_price;
            ti.exit_price = t.exit_price;
            ti.pnl = t.net_profit;
            ti.exit_reason = t.exit_reason; 
            ti.stop_history = t.stop_events;
            
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
            ti.candle_idx = 0; ti.exit_candle_idx = 0;
        } else {
            size_t max_idx = result.history_1m.size() - 1;
            if (ti.candle_idx > max_idx) ti.candle_idx = max_idx;
            if (ti.exit_candle_idx > max_idx) ti.exit_candle_idx = max_idx;
            if (ti.exit_candle_idx < ti.candle_idx) ti.exit_candle_idx = ti.candle_idx;
        }
    }

    return result;
}