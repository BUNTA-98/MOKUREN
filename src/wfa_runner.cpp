#include "wfa_runner.hpp"

#include "config_parser.hpp"
#include "data_manager.hpp"
#include "factory.hpp"
#include "logger.hpp"
#include "market_context.hpp"
#include "performance_metrics.hpp"
#include "prop_equity.hpp"
#include "prop_guard.hpp"
#include "replay_manager.hpp"
#include "runner_common.hpp"
#include "signal_executor.hpp"
#include "trade_event.hpp"
#include "trade_management.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mokuren {

void WfaRunner::RunWFA(const std::string &config_path,
                       StatusCallback on_status,
                       ProgressCallback on_progress,
                       ResultCallback on_result,
                       const std::string &dataset_path) {
    nlohmann::json base_json;
    try {
        std::ifstream file(config_path);
        if (file.is_open()) base_json = nlohmann::json::parse(file);
    } catch (...) {}

    // same pre-flight as the scan: report a broken config as a config error,
    // not as an empty dataset further down.
    try {
        // a backtest never signs a request, so a blank api_key is not an error
        ValidateRunConfig(base_json, "wfa", false);
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
    on_status("LOADING DATASET FOR WFA: " + active_dataset);
    
    // a damaged recording throws from the loader. surface the reason on the
    // status line instead of running every window on a book-blind file.
    std::vector<TradeEvent> all_trades;
    std::vector<L2Snapshot> all_l2;
    try {
        // see grid_runner: a single pass, so a .mkn is decoded once and both
        // channels come from the same state of the file.
        DataManager::LoadedDataset dataset = DataManager::LoadAll(active_dataset);
        all_trades = std::move(dataset.trades);
        all_l2 = std::move(dataset.l2);
    } catch (const std::exception& e) {
        LOG_ERROR("wfa: {}", e.what());
        on_status(std::string("ERR: DATASET: ") + e.what());
        return;
    }

    if (all_trades.empty()) {
        LOG_ERROR("wfa: no trades found in dataset {}", active_dataset);
        on_status("ERR: NO TRADES FOUND IN DATASET");
        return;
    }

    if (!DataManager::ValidateDataAlignment(all_trades, all_l2, on_status)) return;

    LOG_INFO("wfa start: dataset={} trades={}", active_dataset, all_trades.size());

    int64_t first_ts = all_trades.front().timestamp;
    int64_t last_ts = all_trades.back().timestamp;

    // get wfa params
    int64_t in_days = base_json.value("backtest", nlohmann::json::object()).value("wfa_in_sample_days", 14);
    int64_t out_days = base_json.value("backtest", nlohmann::json::object()).value("wfa_out_of_sample_days", 7);
    int64_t step_days = base_json.value("backtest", nlohmann::json::object()).value("wfa_step_days", 7);
    
    const unsigned int max_cores = ReadMaxCores(base_json);

    // conv days to ms
    int64_t day_ms = 86400000LL; 
    int64_t in_sample_ms = in_days * day_ms;    
    int64_t out_of_sample_ms = out_days * day_ms; 
    int64_t step_ms = step_days * day_ms;          

    std::vector<WFAWindow> windows = DataManager::GenerateWFAWindows(first_ts, last_ts, in_sample_ms, out_of_sample_ms, step_ms);
    if (windows.empty()) {
        LOG_ERROR("wfa: dataset too short (needs {} days)", in_days + out_days);
        on_status("ERR: CSV TOO SHORT (NEEDS " + std::to_string(in_days + out_days) + " DAYS)");
        return;
    }
    LOG_INFO("wfa: generated {} rolling windows (in={}d out={}d step={}d)",
             windows.size(), in_days, out_days, step_days);
    on_status("WFA: GENERATED " + std::to_string(windows.size()) + " ROLLING WINDOWS");

    std::vector<TradeRecord> global_oos_trades;
    std::vector<WFAOOSConfig> collected_configs;
    bool oos_any_breached = false;

    // run wfa windows seq
    for (size_t i = 0; i < windows.size(); ++i) {
        const auto& win = windows[i];
        LOG_INFO("wfa window {}/{}: is=[{},{}] oos=[{},{}]",
                 i + 1, windows.size(),
                 win.in_sample_start, win.in_sample_end,
                 win.out_of_sample_start, win.out_of_sample_end);
        on_status("WFA WINDOW " + std::to_string(i + 1) + "/" + std::to_string(windows.size()) + " (PARALLEL)");

        // isolate window data via data mgr
        std::vector<TradeEvent> in_sample_data = DataManager::SliceTrades(all_trades, win.in_sample_start, win.in_sample_end);
        std::vector<TradeEvent> out_of_sample_data = DataManager::SliceTrades(all_trades, win.out_of_sample_start, win.out_of_sample_end);
        std::vector<L2Snapshot> in_sample_l2 = DataManager::SliceL2(all_l2, win.in_sample_start, win.in_sample_end);
        std::vector<L2Snapshot> out_of_sample_l2 = DataManager::SliceL2(all_l2, win.out_of_sample_start, win.out_of_sample_end);

        if (base_json.empty()) continue; 
        std::vector<RunConfig> runs = GridScanner::GenerateGrid(base_json);

        double best_in_sample_profit = -999999.0;
        if (runs.empty()) {
            LOG_WARN("wfa window {}: empty grid, skipping", i + 1);
            on_progress(i + 1, windows.size());
            continue;
        }
        RunConfig best_config = runs[0];
        // carried alongside the winner so the UI/CSV can flag a config that
        // would have ended the account. selection stays profit-based; the
        // flag is a human decision, not an automatic override.
        bool best_is_breached = false;

        std::mutex best_mutex;
        std::atomic<size_t> task_index{0};
        std::vector<std::thread> workers;

        // exec in-sample parallel
        for (unsigned int c = 0; c < max_cores; ++c) {
            workers.emplace_back([&]() {
                while (true) {
                    size_t idx = task_index.fetch_add(1);
                    if (idx >= runs.size()) break;

                    try {
                    const auto& run = runs[idx];
                    // same reason as ValidateRunConfig above: no broker is
                    // built on this path, so credentials are irrelevant to
                    // the run.
                    AppConfig cfg = AppConfig::Load(run.full_json, false);
                    
                    const mokuren::RiskPcts risk = mokuren::ResolveRiskPcts(run.full_json);
                    cfg.sl_pct = risk.sl;
                    cfg.tp_pct = risk.tp;
                    
                    EngineInstance eng = StrategyFactory::Build(cfg);
                    ApplyRMultiplesManagement(run.full_json, cfg.sl_pct, eng);

                    // the same funnel the live worker uses.
                    mokuren::SignalExecutor executor(
                        eng.ptrader.get(), eng.sizer.get(),
                        eng.risk_manager.get(), cfg.sl_pct, cfg.tp_pct,
                        "wfa_in_sample");

                    // one guard per permutation, seeded from the paper
                    // account's starting balance.
                    mokuren::PropRules prop_rules = cfg.prop;
                    if (prop_rules.initial_balance <= 0.0) {
                        prop_rules.initial_balance = PaperTrader::kInitialBalance;
                    }
                    mokuren::PropGuard prop_guard(prop_rules);
                    // see grid_runner: the gate must not rely on ObserveProp
                    // having armed a disabled guard before the first signal.
                    const bool prop_on = prop_rules.enabled;
                    bool prop_breached = false;

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

                            const double mark = in_sample_l2[l2_idx].best_bid_price > 0.0
                                                    ? in_sample_l2[l2_idx].best_bid_price
                                                    : trade.price;
                            const mokuren::PropDecision pd = mokuren::ObserveProp(
                                prop_guard, eng.ptrader.get(), mark,
                                in_sample_l2[l2_idx].timestamp, "wfa_is");
                            if (pd.state == mokuren::PropState::DailyHalted ||
                                pd.state == mokuren::PropState::TotalHalted) {
                                prop_breached = true;
                            }

                            TradeSignal tick_sig = eng.alpha->Evaluate(tick_ctx, false);
                            if (tick_sig.direction != SignalDirection::NONE &&
                                (!prop_on || prop_guard.allow_new_entry())) {
                                double exec_price = (tick_sig.direction == SignalDirection::BUY) 
                                                  ? in_sample_l2[l2_idx].best_ask_price 
                                                  : in_sample_l2[l2_idx].best_bid_price;
                                
                                if (exec_price <= 0.0) exec_price = live_bar.close > 0.0 ? live_bar.close : trade.price;
                                executor.Execute(tick_sig, exec_price, in_sample_l2[l2_idx].timestamp);
                            }
                            l2_idx++;
                        }

                        // observe the guard before anything can act on this
                        // print: a breach must refuse the trade, not race it.
                        const mokuren::PropDecision pd = mokuren::ObserveProp(
                            prop_guard, eng.ptrader.get(), trade.price,
                            trade.timestamp, "wfa_is");
                        if (pd.state == mokuren::PropState::DailyHalted ||
                            pd.state == mokuren::PropState::TotalHalted) {
                            prop_breached = true;
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
                        if (!prop_on || prop_guard.allow_new_entry()) {
                            executor.Execute(candle_sig, trade.price, trade.timestamp);
                        }
                    }
                    
                    eng.aggregator->FlushLastCandle(live_bar);
                    eng.ptrader->CloseOpenPositionAtEnd(live_bar.close, live_bar.timestamp_start);

                    double profit = eng.ptrader->GetNetProfit();
                    
                    // track best is run
                    std::lock_guard<std::mutex> lock(best_mutex);
                    if (profit > best_in_sample_profit) {
                        best_in_sample_profit = profit;
                        best_config = run;
                        best_is_breached = prop_breached;
                    }
                    } catch (const std::exception& e) {
                        LOG_ERROR("wfa worker: run {} threw: {}", idx, e.what());
                    } catch (...) {
                        LOG_ERROR("wfa worker: run {} threw an unknown exception", idx);
                    }
                }
            });
        }

        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

        // exec winning config in oos
        AppConfig oos_cfg = AppConfig::Load(best_config.full_json, false);
        const mokuren::RiskPcts oos_risk = mokuren::ResolveRiskPcts(best_config.full_json);
        oos_cfg.sl_pct = oos_risk.sl;
        oos_cfg.tp_pct = oos_risk.tp;

        EngineInstance oos_eng = StrategyFactory::Build(oos_cfg);
        ApplyRMultiplesManagement(best_config.full_json, oos_cfg.sl_pct, oos_eng);

        mokuren::SignalExecutor executor(
            oos_eng.ptrader.get(), oos_eng.sizer.get(),
            oos_eng.risk_manager.get(), oos_cfg.sl_pct, oos_cfg.tp_pct,
            "wfa_out_of_sample");

        // the OOS run gets its own guard: the winner's in-sample breach flag
        // is a property of the IS run, not of the OOS slice.
        mokuren::PropRules oos_prop_rules = oos_cfg.prop;
        if (oos_prop_rules.initial_balance <= 0.0) {
            oos_prop_rules.initial_balance = PaperTrader::kInitialBalance;
        }
        mokuren::PropGuard oos_prop_guard(oos_prop_rules);
        const bool oos_prop_on = oos_prop_rules.enabled;
        bool oos_prop_breached = false;

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

                const double mark = out_of_sample_l2[l2_oos_idx].best_bid_price > 0.0
                                        ? out_of_sample_l2[l2_oos_idx].best_bid_price
                                        : trade.price;
                const mokuren::PropDecision pd = mokuren::ObserveProp(
                    oos_prop_guard, oos_eng.ptrader.get(), mark,
                    out_of_sample_l2[l2_oos_idx].timestamp, "wfa_oos");
                if (pd.state == mokuren::PropState::DailyHalted ||
                    pd.state == mokuren::PropState::TotalHalted) {
                    oos_prop_breached = true;
                }

                TradeSignal tick_sig = oos_eng.alpha->Evaluate(tick_ctx, false);
                if (tick_sig.direction != SignalDirection::NONE &&
                    (!oos_prop_on || oos_prop_guard.allow_new_entry())) {
                    double exec_price = (tick_sig.direction == SignalDirection::BUY) 
                                      ? out_of_sample_l2[l2_oos_idx].best_ask_price 
                                      : out_of_sample_l2[l2_oos_idx].best_bid_price;
                    
                    if (exec_price <= 0.0) exec_price = oos_live.close > 0.0 ? oos_live.close : trade.price;
                    executor.Execute(tick_sig, exec_price, out_of_sample_l2[l2_oos_idx].timestamp);
                }
                l2_oos_idx++;
            }

            // observe the guard before anything can act on this print.
            const mokuren::PropDecision pd = mokuren::ObserveProp(
                oos_prop_guard, oos_eng.ptrader.get(), trade.price,
                trade.timestamp, "wfa_oos");
            if (pd.state == mokuren::PropState::DailyHalted ||
                pd.state == mokuren::PropState::TotalHalted) {
                oos_prop_breached = true;
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
            if (!oos_prop_on || oos_prop_guard.allow_new_entry()) {
                executor.Execute(candle_sig, trade.price, trade.timestamp);
            }
        }
        
        oos_eng.aggregator->FlushLastCandle(oos_live);
        oos_eng.ptrader->CloseOpenPositionAtEnd(oos_live.close, oos_live.timestamp_start);

        // agg oos trades for global rep
        auto* oos_pt = dynamic_cast<PaperTrader*>(oos_eng.ptrader.get());
        // GetTradeHistory() returns by value now: name the vector so the copy
        // is explicit instead of hidden behind a reference binding.
        const std::vector<TradeRecord> window_trades =
            oos_pt->GetTradeHistory();
        global_oos_trades.insert(global_oos_trades.end(), window_trades.begin(), window_trades.end());
        
        // save config for ui replay
        collected_configs.push_back({win.out_of_sample_start, win.out_of_sample_end, best_config.full_json});
        LOG_INFO("wfa window {}/{}: best_is_profit={} is_breached={} oos_trades={} oos_breached={}",
                 i + 1, windows.size(), best_in_sample_profit, best_is_breached,
                 window_trades.size(), oos_prop_breached);
        if (oos_prop_breached) oos_any_breached = true;
        on_progress(i + 1, windows.size());
    } 

    // comp agg wfa res. a window whose OOS run breached the account limits
    // before it could fill a single trade is still a reportable outcome: the
    // whole point of the flag is that this config is untradeable.
    if (!global_oos_trades.empty() || oos_any_breached) {
        TestResult res{};
        res.parameters["WFA_Windows_Done"] = windows.size(); 
        
        nlohmann::json final_cfg = collected_configs.back().config;
        final_cfg["backtest"]["wfa_mode"] = true; 
        res.full_config = final_cfg; 
        
        res.trades = global_oos_trades.size();
        res.trade_log = global_oos_trades;
        res.wfa_configs = collected_configs; 
        // the aggregate OOS result is flagged if any window's OOS run
        // breached a limit. the per-window IS flag is logged above; the
        // aggregate result is what the UI/CSV surface.
        res.prop_breached = oos_any_breached;
        
        double current_balance = PaperTrader::kInitialBalance;
        double peak_balance = PaperTrader::kInitialBalance;
        double max_dd = 0.0, net_profit = 0.0;
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

        LOG_INFO("wfa complete: trades={} net={} dd={} winrate={} sharpe={} prop_breached={}",
                 res.trades, res.net_profit, res.max_drawdown,
                 res.winrate, res.sharpe, res.prop_breached);

        on_result(res);
    }

    on_status("WFA COMPLETE");
}

ReplayResult WfaRunner::ReplayWFA(const std::vector<WFAOOSConfig>& wfa_configs,
                                  const std::string& dataset_path) {
    ReplayResult result{};
    if (wfa_configs.empty()) return result;

    // the caller may have resolved the dataset from the selected profile; only
    // fall back to the first window's config when it did not.
    std::string active_dataset = dataset_path;
    if (active_dataset.empty()) {
        active_dataset = wfa_configs[0].config.value("environment", nlohmann::json::object()).value("dataset", "data");
    }
    LOG_INFO("replay wfa: dataset={} windows={}", active_dataset, wfa_configs.size());

    // a replay is a diagnostic: report a damaged recording and return the
    // empty result instead of unwinding through the UI that asked for it.
    std::vector<TradeEvent> all_trades;
    std::vector<L2Snapshot> all_l2;
    try {
        DataManager::LoadedDataset dataset = DataManager::LoadAll(active_dataset);
        all_trades = std::move(dataset.trades);
        all_l2 = std::move(dataset.l2);
    } catch (const std::exception& e) {
        LOG_ERROR("replay wfa: {}", e.what());
        return result;
    }

    if (all_trades.empty()) {
        LOG_ERROR("replay wfa: no trades found in dataset {}", active_dataset);
        return result;
    }

    for (const auto& wfa_cfg : wfa_configs) {
        // replay is a backtest: no broker, no signed requests.
        AppConfig cfg = AppConfig::Load(wfa_cfg.config, false);
        const mokuren::RiskPcts wfa_risk = mokuren::ResolveRiskPcts(wfa_cfg.config);
        cfg.sl_pct = wfa_risk.sl;
        cfg.tp_pct = wfa_risk.tp;

        EngineInstance eng = StrategyFactory::Build(cfg);
        ApplyRMultiplesManagement(wfa_cfg.config, cfg.sl_pct, eng);

        mokuren::SignalExecutor executor(
            eng.ptrader.get(), eng.sizer.get(), eng.risk_manager.get(),
            cfg.sl_pct, cfg.tp_pct, "replay_wfa");

        // replay parity: the same guard the WFA-OOS run used, so a replay
        // reproduces the run that produced the config.
        mokuren::PropRules replay_prop_rules = cfg.prop;
        if (replay_prop_rules.initial_balance <= 0.0) {
            replay_prop_rules.initial_balance = PaperTrader::kInitialBalance;
        }
        mokuren::PropGuard replay_prop_guard(replay_prop_rules);
        const bool replay_prop_on = replay_prop_rules.enabled;
        // the replay flattens on a breach inside ObserveProp; latch the state
        // so the caller can see it instead of the flag being discarded.
        bool replay_prop_breached = false;

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

                const double mark = oos_l2[l2_idx].best_bid_price > 0.0
                                        ? oos_l2[l2_idx].best_bid_price
                                        : trade.price;
                const mokuren::PropDecision pd = mokuren::ObserveProp(
                    replay_prop_guard, eng.ptrader.get(), mark,
                    oos_l2[l2_idx].timestamp, "replay_wfa");
                if (pd.state == mokuren::PropState::DailyHalted ||
                    pd.state == mokuren::PropState::TotalHalted) {
                    replay_prop_breached = true;
                }

                TradeSignal tick_sig = eng.alpha->Evaluate(tick_ctx, false);
                if (tick_sig.direction != SignalDirection::NONE &&
                    (!replay_prop_on || replay_prop_guard.allow_new_entry())) {
                    double exec_price = (tick_sig.direction == SignalDirection::BUY) 
                                      ? oos_l2[l2_idx].best_ask_price 
                                      : oos_l2[l2_idx].best_bid_price;
                    
                    if (exec_price <= 0.0) exec_price = live_bar.close > 0.0 ? live_bar.close : trade.price;
                    executor.Execute(tick_sig, exec_price, oos_l2[l2_idx].timestamp);
                }
                l2_idx++;
            }

            // observe the guard before anything can act on this print.
            const mokuren::PropDecision pd = mokuren::ObserveProp(
                replay_prop_guard, eng.ptrader.get(),
                trade.price, trade.timestamp, "replay_wfa");
            if (pd.state == mokuren::PropState::DailyHalted ||
                pd.state == mokuren::PropState::TotalHalted) {
                replay_prop_breached = true;
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
            if (!replay_prop_on || replay_prop_guard.allow_new_entry()) {
                executor.Execute(candle_sig, trade.price, trade.timestamp);
            }
        }
        eng.aggregator->FlushLastCandle(live_bar);
        eng.ptrader->CloseOpenPositionAtEnd(live_bar.close, live_bar.timestamp_start);

        // a breach in any window makes the aggregate replay untradeable, the
        // same way oos_any_breached flags the aggregate WFA result.
        if (replay_prop_breached) result.prop_breached = true;

        // concat hist seq
        const auto& hist_1m = eng.aggregator->GetHistory();
        const auto& hist_15m = eng.htf_aggregator->GetHistory();
        size_t start_idx_1m = result.history_1m.size();
        
        result.history_1m.insert(result.history_1m.end(), hist_1m.begin(), hist_1m.end());
        result.history_15m.insert(result.history_15m.end(), hist_15m.begin(), hist_15m.end());

        // map ui data for concat
        const std::vector<TradeRecord> trade_log = eng.ptrader->GetTradeHistory();
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

    LOG_INFO("replay wfa complete: bars_1m={} bars_15m={} trades={} prop_breached={}",
             result.history_1m.size(), result.history_15m.size(),
             result.trades.size(), result.prop_breached);

    return result;
}

} // namespace mokuren
