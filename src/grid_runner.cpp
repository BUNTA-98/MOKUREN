#include "grid_runner.hpp"

#include "config_parser.hpp"
#include "data_manager.hpp"
#include "factory.hpp"
#include "logger.hpp"
#include "market_context.hpp"
#include "performance_metrics.hpp"
#include "prop_equity.hpp"
#include "prop_guard.hpp"
#include "runner_common.hpp"
#include "signal_executor.hpp"
#include "trade_event.hpp"
#include "trade_management.hpp"

#include <atomic>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace mokuren {

void GridRunner::Run(const std::string &config_path,
                     StatusCallback on_status,
                     ProgressCallback on_progress,
                     ResultCallback on_result,
                     const std::string &dataset_path) {
    std::ifstream file(config_path);
    if (!file.is_open()) {
        LOG_ERROR("grid search: config not found: {}", config_path);
        on_status("ERROR: CONFIG NOT FOUND");
        return;
    }
    
    // parse base json
    nlohmann::json base_json = nlohmann::json::parse(file);
    
    // gen params
    std::vector<RunConfig> runs = GridScanner::GenerateGrid(base_json);

    if (runs.empty()) {
        LOG_ERROR("grid search: no permutations generated from {}", config_path);
        on_status("ERR: EMPTY GRID");
        return;
    }
    // validate once, on the driver thread. a config error inside a worker was
    // caught by the per-run handler and reported as "no trades".
    try {
        ValidateRunConfig(runs.front().full_json, "grid search");
    } catch (const std::exception& e) {
        on_status(std::string("ERR: CONFIG: ") + e.what());
        return;
    }
    
    // an explicit path wins over the config: it is the one the operator
    // actually selected. the fallback keeps CLI/test callers working.
    std::string active_dataset = dataset_path;
    if (active_dataset.empty()) {
        active_dataset = base_json.value("environment", nlohmann::json::object()).value("dataset", "data");
    }
    LOG_INFO("grid search start: config={} dataset={} permutations={}",
             config_path, active_dataset, runs.size());
    on_status("LOADING DATASET: " + active_dataset);
    
    // load full dataset to ram via data manager.
    //
    // a damaged recording throws instead of returning a short vector; the
    // message goes to the status line so the operator sees the reason, not a
    // scan that quietly evaluated nothing.
    std::vector<TradeEvent> all_trades;
    std::vector<L2Snapshot> all_l2;
    try {
        all_trades = DataManager::LoadAllTrades(active_dataset);
        all_l2 = DataManager::LoadAllL2Snapshots(active_dataset);
    } catch (const std::exception& e) {
        LOG_ERROR("grid search: {}", e.what());
        on_status(std::string("ERR: DATASET: ") + e.what());
        return;
    }

    if (all_trades.empty()) {
        LOG_ERROR("grid search: no trades found in dataset {}", active_dataset);
        on_status("ERR: NO TRADES FOUND IN DATASET");
        return;
    }

    if (all_l2.empty()) {
        LOG_WARN("grid search: no L2 data found for {}", active_dataset);
        on_status("WARN: NO L2 DATA FOUND");
    } else {
        if (!DataManager::ValidateDataAlignment(all_trades, all_l2, on_status)) return;
    }

    // setup threads
    const unsigned int max_cores = ReadMaxCores(base_json);
    
    LOG_INFO("grid search: {} trades, {} l2 snapshots, {} threads",
             all_trades.size(), all_l2.size(), max_cores);
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

    // UI callbacks are not thread-safe; serialise them across workers.
    std::mutex ui_mutex;

    // spawn workers
    std::vector<std::thread> workers;
    for (unsigned int i = 0; i < max_cores; ++i) {
        workers.emplace_back([&]() {
            while (true) {
                // fetch next job atomic
                size_t idx = task_index.fetch_add(1);
                if (idx >= runs.size()) break; 
                
                // a throwing strategy or malformed json must not escape a
                // worker: that calls std::terminate and kills the whole scan.
                try {
                const auto& run = runs[idx];
                AppConfig cfg = AppConfig::Load(run.full_json);
                
                // get sl and tp
                const mokuren::RiskPcts risk = mokuren::ResolveRiskPcts(run.full_json);
                cfg.sl_pct = risk.sl;
                cfg.tp_pct = risk.tp;
                
                // init strat mods
                EngineInstance eng = StrategyFactory::Build(cfg);
                ApplyRMultiplesManagement(run.full_json, cfg.sl_pct, eng);

                // one funnel, shared with live and every other backtest path:
                // stop validation, sizing, the risk firewall, stop
                // re-validation and tp derivation cannot drift between them.
                mokuren::SignalExecutor executor(
                    eng.ptrader.get(), eng.sizer.get(),
                    eng.risk_manager.get(), cfg.sl_pct, cfg.tp_pct, "scan");

                // one guard per permutation, seeded from the paper account's
                // starting balance so total drawdown is measured against the
                // same base the live path uses.
                mokuren::PropRules prop_rules = cfg.prop;
                if (prop_rules.initial_balance <= 0.0) {
                    prop_rules.initial_balance = PaperTrader::kInitialBalance;
                }
                mokuren::PropGuard prop_guard(prop_rules);
                // a config without a prop block keeps the old behaviour. the
                // gates below must not depend on ObserveProp arming the guard
                // as a side effect, or a short-circuit there would drop every
                // entry and report the scan as "no setups".
                const bool prop_on = prop_rules.enabled;
                bool prop_breached = false;

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

                        // observe the guard on every L2 tick too: a floating
                        // loss can cross a limit on a quiet book with no
                        // prints, and the live path has the same gap.
                        const double mark = all_l2[l2_idx].best_bid_price > 0.0
                                                ? all_l2[l2_idx].best_bid_price
                                                : trade.price;
                        const mokuren::PropDecision pd = mokuren::ObserveProp(
                            prop_guard, eng.ptrader.get(), mark,
                            all_l2[l2_idx].timestamp, "scan");
                        if (pd.state == mokuren::PropState::DailyHalted ||
                            pd.state == mokuren::PropState::TotalHalted) {
                            prop_breached = true;
                        }

                        // eval tick sigs
                        TradeSignal tick_sig = eng.alpha->Evaluate(tick_ctx, false);
                        
                        if (tick_sig.direction != SignalDirection::NONE &&
                            (!prop_on || prop_guard.allow_new_entry())) {
                            // cross spread for hft exec
                            double exec_price = (tick_sig.direction == SignalDirection::BUY) 
                                              ? all_l2[l2_idx].best_ask_price 
                                              : all_l2[l2_idx].best_bid_price;
                            
                            // fallback if no price
                            if (exec_price <= 0.0) exec_price = live_bar.close > 0.0 ? live_bar.close : trade.price;
                            
                            executor.Execute(tick_sig, exec_price, all_l2[l2_idx].timestamp);
                        }
                        
                        l2_idx++;
                    }

                    // observe the guard before anything can act on this
                    // print: a breach must refuse the trade, not race it.
                    const mokuren::PropDecision pd = mokuren::ObserveProp(
                        prop_guard, eng.ptrader.get(), trade.price,
                        trade.timestamp, "scan");
                    if (pd.state == mokuren::PropState::DailyHalted ||
                        pd.state == mokuren::PropState::TotalHalted) {
                        prop_breached = true;
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
                    if (!prop_on || prop_guard.allow_new_entry()) {
                        executor.Execute(candle_sig, trade.price, trade.timestamp);
                    }
                }

                // force close at end
                eng.aggregator->FlushLastCandle(live_bar);
                eng.ptrader->CloseOpenPositionAtEnd(live_bar.close, live_bar.timestamp_start);

                // compile metrics. a breach with zero trades is still a
                // result: a prop.initial_balance that does not match the paper
                // account halts the guard on the first observation, and
                // filtering those permutations out by "trades > 0" would hide
                // exactly the configs the guard exists to reject.
                if (eng.ptrader->GetTotalTrades() > 0 || prop_breached) {
                    TestResult res{};
                    res.parameters = run.grid_values;
                    res.full_config = run.full_json; 
                    res.net_profit = eng.ptrader->GetNetProfit();
                    res.max_drawdown = eng.ptrader->GetMaxDrawdown(); 
                    res.trades = eng.ptrader->GetTotalTrades();
                    // GetWinrate divides by the trade count. a breach before
                    // the first fill has an empty log, so take the zero case
                    // here instead of relying on the broker to special-case it.
                    res.winrate = res.trades > 0 ? eng.ptrader->GetWinrate() : 0.0;
                    res.trade_log = eng.ptrader->GetTradeHistory();
                    res.prop_breached = prop_breached;
                    
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

                    LOG_INFO("grid result: trades={} net={} dd={} winrate={} sharpe={} prop_breached={}",
                             res.trades, res.net_profit, res.max_drawdown,
                             res.winrate, res.sharpe, res.prop_breached);
                    
                    {
                        std::lock_guard<std::mutex> lock(ui_mutex);
                        on_result(res);
                    }

                    // safe csv write
                    {
                        std::lock_guard<std::mutex> lock(csv_mutex);
                        if (csv_file.is_open()) {
                            // write dyn headers once
                            if (!header_written) {
                                for (const auto& [key, val] : run.grid_values) {
                                    csv_file << key << ",";
                                }
                                csv_file << "NetProfit,MaxDrawdown,Trades,WinRate,Sharpe,PropBreached\n";
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
                                     << res.sharpe << ","
                                     << (res.prop_breached ? 1 : 0) << "\n";
                            csv_file.flush();
                        }
                    }
                }
                
                int done = ++current_iteration;
                {
                    std::lock_guard<std::mutex> lock(ui_mutex);
                    on_progress(done, total_iterations);
                }
                } catch (const std::exception& e) {
                    LOG_ERROR("grid worker: run {} threw: {}", idx, e.what());
                } catch (...) {
                    LOG_ERROR("grid worker: run {} threw an unknown exception", idx);
                }
            }
        });
    }

    // wait for workers
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }
    
    LOG_INFO("grid search complete: {} permutations evaluated", total_iterations);
    on_status("SCAN COMPLETE");
}

} // namespace mokuren
