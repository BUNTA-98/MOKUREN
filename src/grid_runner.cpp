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
#include <cmath>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
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
    // Validate the whole grid on the driver thread, before a single backtest
    // starts. Only the front permutation used to be checked, so a grid axis
    // that steps outside what AppConfig accepts (e.g. risk.tp_pct >= 0.1)
    // surfaced only inside a worker, where the per-run catch treated it as a
    // dead permutation: the scan advertised 10, ran 4, and still ended with
    // "SCAN COMPLETE". The operator saw a stalled bar and a half-empty
    // leaderboard instead of the one config value that was wrong.
    {
        size_t bad = 0;
        int first_index = -1;
        std::string first_error;
        std::string first_values;
        for (size_t i = 0; i < runs.size(); ++i) {
            try {
                // a backtest never signs a request, so a blank api_key is not
                // an error. Load() is what enforces the risk block's ranges.
                const AppConfig probe = AppConfig::Load(runs[i].full_json, false);
                (void)probe;
            } catch (const std::exception& e) {
                if (bad == 0) {
                    first_index = static_cast<int>(i);
                    first_error = e.what();
                    // name the swept values, not only the index. "first #4"
                    // left the operator to count steps in their head; the
                    // axis and value that AppConfig refused is what they need
                    // to change.
                    std::ostringstream vals;
                    const char* sep = "";
                    for (const auto& [key, value] : runs[i].grid_values) {
                        vals << sep << key << '=' << value;
                        sep = ", ";
                    }
                    first_values = vals.str();
                }
                ++bad;
                LOG_ERROR("grid search: permutation {} invalid: {}", i, e.what());
            }
        }
        if (bad > 0) {
            const std::string msg =
                "ERR: CONFIG: " + std::to_string(bad) + "/" +
                std::to_string(runs.size()) + " permutations invalid (first #" +
                std::to_string(first_index) + " [" + first_values + "]: " +
                first_error + ")";
            LOG_ERROR("grid search: {}", msg);
            on_status(msg);
            return;
        }
        // ValidateRunConfig still runs: it covers the rules that are not part
        // of AppConfig::Load, and the front permutation is the cheapest place
        // to surface them.
        try {
            ValidateRunConfig(runs.front().full_json, "grid search", false);
        } catch (const std::exception& e) {
            on_status(std::string("ERR: CONFIG: ") + e.what());
            return;
        }
    }
    
    // an explicit path wins over the config: it is the one the operator
    // actually selected. the fallback keeps CLI/test callers working.
    std::string active_dataset = dataset_path;
    if (active_dataset.empty()) {
        // no silent "data" fallback: that default is what turned a config
        // pointing at a .mkn the operator picked into a scan of the directory
        // "data", and the failure read as "no trades found".
        const auto env = base_json.value("environment", nlohmann::json::object());
        active_dataset = env.value("dataset", env.value("filepath", std::string{}));
    }
    if (active_dataset.empty()) {
        LOG_ERROR("grid search: no dataset configured");
        on_status("ERR: NO DATASET CONFIGURED");
        return;
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
        // one pass over the dataset. for a .mkn recording this is the
        // difference between decoding the file once and decoding it twice.
        DataManager::LoadedDataset dataset = DataManager::LoadAll(active_dataset);
        all_trades = std::move(dataset.trades);
        all_l2 = std::move(dataset.l2);
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
    std::atomic<int> failed_iterations{0};

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
                int sig_seen = 0, sig_exec = 0, sig_open = 0, sig_veto = 0;
                // same reason as ValidateRunConfig above: no broker is built
                // on this path, so credentials are irrelevant to the run.
                AppConfig cfg = AppConfig::Load(run.full_json, false);
                
                // sl/tp are the values Load just resolved and Validate
                // approved. This used to re-derive them through
                // ResolveRiskPcts, a second reader of the same keys: a
                // disagreement about precedence (sl_pct vs legacy
                // sl_price_pct, tp_r vs tp_pct) would run one risk distance
                // while validating another, and ApplyRMultiplesManagement --
                // already scaled by Load's sl_pct -- would keep the other.
                
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
                // the base total drawdown is measured against is the SAME
                // number that funds the paper account, so the two cannot
                // disagree. it used to travel on PropRules, where a second
                // value (or 0) could silently win and report drawdown against
                // an account this run never funded.
                const mokuren::PropRules& prop_rules = cfg.prop;
                mokuren::PropGuard prop_guard(prop_rules, cfg.initial_balance);
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
                    // catch up l2 -- strict `<`, matching ReplayManager::Run.
                    // A book frame sharing this print's millisecond is applied
                    // at the top of the next iteration, not before this print
                    // is evaluated. live_streamer.hpp guarantees the print is
                    // emitted first at identical timestamps, so `<=` handed
                    // the strategy a book the live run had not seen yet.
                    while (l2_idx < all_l2.size() && all_l2[l2_idx].timestamp < trade.timestamp) {
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
                        const L2Snapshot& snap = all_l2[l2_idx];
                        const mokuren::PropDecision pd = mokuren::ObservePropBook(
                            prop_guard, eng.ptrader.get(),
                            snap.best_bid_price, snap.best_ask_price, trade.price,
                            snap.timestamp, "scan");
                        if (pd.state == mokuren::PropState::DailyHalted ||
                            pd.state == mokuren::PropState::TotalHalted) {
                            prop_breached = true;
                        }

                        // eval tick sigs
                        TradeSignal tick_sig = eng.alpha->Evaluate(tick_ctx, false);
                        
                        if (tick_sig.direction != SignalDirection::NONE) {
                            ++sig_seen;
                            if (!prop_on || prop_guard.allow_new_entry()) {
                                // cross spread for hft exec
                                double exec_price = (tick_sig.direction == SignalDirection::BUY) 
                                                  ? all_l2[l2_idx].best_ask_price 
                                                  : all_l2[l2_idx].best_bid_price;
                                
                                // fallback if no price
                                if (exec_price <= 0.0) exec_price = live_bar.close > 0.0 ? live_bar.close : trade.price;
                                
                                auto r = executor.Execute(tick_sig, exec_price, all_l2[l2_idx].timestamp);
                                if (r.signal.direction != SignalDirection::NONE) ++sig_exec;
                                if (r.opened_position) ++sig_open;
                            } else {
                                ++sig_veto;
                            }
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
                    if (candle_sig.direction != SignalDirection::NONE) {
                        ++sig_seen;
                        if (!prop_on || prop_guard.allow_new_entry()) {
                            auto r = executor.Execute(candle_sig, trade.price, trade.timestamp);
                            if (r.signal.direction != SignalDirection::NONE) ++sig_exec;
                            if (r.opened_position) ++sig_open;
                        } else {
                            ++sig_veto;
                        }
                    }
                }

                // force close at end
                eng.aggregator->FlushLastCandle(live_bar);
                eng.ptrader->CloseOpenPositionAtEnd(live_bar.close, live_bar.timestamp_start);

                // compile metrics. every permutation records a row now,
                // including the ones that never filled. The old
                // "trades > 0 || prop_breached" gate made two very different
                // outcomes look identical -- "the setup never fired" and "the
                // permutation threw" both left the leaderboard empty, while
                // the scan still counted them apart. `trades` (0) is the
                // column that separates them. A breach is still recorded
                // because a prop.initial_balance that does not match the paper
                // account halts the guard on the first observation, and those
                // are exactly the configs the guard exists to reject.
                //
                // the block stays scoped so `res` does not leak into the
                // worker's next iteration.
                {
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

                    // calc adv metrics. only over an actual sample: the ratios
                    // are undefined with zero returns, and AdvancedMetrics'
                    // defaults (0.0) are honest where a NaN or a divide-by-zero
                    // would poison the leaderboard's sort. a zero-trade row
                    // therefore reports sharpe/sortino/mc_dd as 0, exactly as
                    // it did when it was filtered out -- it just shows up now.
                    if (!res.trade_log.empty()) {
                        AdvancedMetrics am = PerformanceMetrics::CalculateAdvancedMetrics(res.trade_log);
                        // a sample with no losing trade has zero downside
                        // deviation, so sortino is +/-inf (or NaN on 0/0).
                        // one non-finite number does not fail a sort, it
                        // silently reorders the whole leaderboard; record 0.0
                        // and let the trade count say why the ratio is absent.
                        res.sharpe = std::isfinite(am.sharpe) ? am.sharpe : 0.0;
                        res.sortino = std::isfinite(am.sortino) ? am.sortino : 0.0;
                        res.mc_drawdown = std::isfinite(am.mc_dd) ? am.mc_dd : 0.0;
                    }

                    // one line per permutation. signals=0 means no trigger
                    // fired (filters or the evaluator); signals>0 with
                    // executed<signals and vetoed==0 means a risk module,
                    // sizer or stop check dropped it -- `[F]` to debug shows
                    // `risk reject: module=...`; vetoed==signals means the
                    // prop guard refused every entry; executed>0 opened==0
                    // means the broker itself dropped it.
                    LOG_INFO("grid perm {}: signals={} executed={} opened={} vetoed={} trades={}",
                             idx, sig_seen, sig_exec, sig_open, sig_veto, res.trades);
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
                
                } catch (const std::exception& e) {
                    failed_iterations.fetch_add(1);
                    LOG_ERROR("grid worker: run {} threw: {}", idx, e.what());
                } catch (...) {
                    failed_iterations.fetch_add(1);
                    LOG_ERROR("grid worker: run {} threw an unknown exception", idx);
                }

                // The progress tick counts every permutation the queue handed
                // out, successful or not. It used to sit inside the try block,
                // so a throwing permutation was logged and then vanished from
                // the bar while its index had already been consumed: the scan
                // reported 4/10 and looked stuck while it was, in fact, done.
                int done = ++current_iteration;
                {
                    std::lock_guard<std::mutex> lock(ui_mutex);
                    on_progress(done, total_iterations);
                }
            }
        });
    }

    // wait for workers
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }
    
    const int evaluated = current_iteration.load();
    const int failed = failed_iterations.load();
    LOG_INFO("grid search complete: {}/{} permutations evaluated, {} failed",
             evaluated, total_iterations, failed);
    if (failed > 0) {
        // "COMPLETE" alone was indistinguishable from a clean scan. A failed
        // permutation still produced no result row, so say so where the
        // operator is actually looking.
        on_status("SCAN COMPLETE (" + std::to_string(evaluated) + "/" +
                  std::to_string(total_iterations) + ", " +
                  std::to_string(failed) + " FAILED - SEE LOG)");
    } else {
        on_status("SCAN COMPLETE");
    }
}

} // namespace mokuren
