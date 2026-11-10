#pragma once
#include "data_manager.hpp"
#include "factory.hpp"
#include "l2_feed.hpp"
#include "logger.hpp"
#include "market_context.hpp"
#include "prop_equity.hpp"
#include "prop_guard.hpp"
#include "run_loop.hpp"
#include "signal_executor.hpp"
#include "trade_event.hpp"
#include "trade_management.hpp"
#include "ui_footprint.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

struct ReplayResult {
    std::vector<Bar> history_1m;
    std::vector<Bar> history_15m;
    std::vector<TradeInfo> trades; 
    // true if the prop guard halted new entries at any point in this replay.
    // the flatten itself already happens inside ObserveProp; this flag is the
    // reporting half, so a replay that would have ended a funded account is
    // visible to the caller instead of looking like a compliant run.
    bool prop_breached = false;
};

class ReplayManager {
public:
    // `dataset_path` is the dataset the caller resolved (the scanner hands over
    // the path from the selected profile); empty falls back to the config's
    // environment.dataset. an explicit path wins, so the prints and the book
    // always come from the same file.
    //
    // throws std::runtime_error when the recording is damaged (see
    // DataManager::LoadAllL2Snapshots). the caller decides whether that is a
    // status line or an empty result; this function does not swallow it.
    //
    // This overload loads the book itself. A caller that already has both
    // channels -- which is every caller holding a .mkn, since one file carries
    // both -- must use the overload below, or the recording is decoded twice.
    static ReplayResult Run(const std::vector<TradeEvent>& all_trades,
                            const nlohmann::json& winning_config,
                            const std::string& dataset_path = "") {
        // No silent "data" fallback. A config pointing at the wrong path used
        // to read ./data and report a quiet market; an empty path is a bad
        // config, and the status line is where that belongs.
        const std::string dataset =
            !dataset_path.empty()
                ? dataset_path
                : winning_config.value("environment", nlohmann::json::object())
                      .value("dataset", std::string{});
        if (dataset.empty()) {
            throw std::runtime_error(
                "replay: no dataset path and no environment.dataset in the "
                "config");
        }

        // Stream a recording, materialise a directory. This overload used to
        // call LoadAllL2Snapshots unconditionally, so replaying the winning
        // config over a week allocated ~14 GB of L2Snapshot -- the same freeze
        // the sweep fix removed from the scan, still present on the path that
        // runs AFTER the sweep picks a winner.
        if (DataManager::IsSessionRecording(dataset)) {
            mokuren::CursorL2Feed book;
            std::string stream_error;
            if (!book.Open(dataset, stream_error)) {
                throw std::runtime_error(stream_error);
            }
            ReplayResult result =
                Run(all_trades, book, winning_config, dataset);
            // A streamed recording discovers a corrupt tail WHILE it is being
            // read, where the materialising loader discovered it before the
            // run started. Fail the replay rather than commit a book-blind
            // result the UI would draw as a quiet market.
            const std::string problem =
                mokuren::L2StreamProblem(book.status());
            if (!problem.empty()) {
                throw std::runtime_error("dataset '" + dataset + "': " +
                                         problem);
            }
            // Same warning the vector overload issues, for the same reason: a
            // run with no book evaluates every tick trigger against nothing
            // and reports no tick signals. L2StreamProblem cannot catch this
            // -- it reports records that were skipped, and an empty channel
            // skipped none -- so it has to be asked of the feed afterwards.
            if (book.frames_delivered() == 0) {
                LOG_WARN("replay: no L2 snapshots for dataset '{}'; tick "
                         "triggers will not fire", dataset);
            }
            return result;
        }
        return Run(all_trades, DataManager::LoadAllL2Snapshots(dataset),
                   winning_config, dataset);
    }

    // Book supplied by the caller. `dataset` is used for logging only; it must
    // name the file the two vectors actually came from, so a replay never
    // evaluates one recording's prints against another recording's book.
    static ReplayResult Run(const std::vector<TradeEvent>& all_trades,
                            const std::vector<L2Snapshot>& all_l2,
                            const nlohmann::json& winning_config,
                            const std::string& dataset) {
        if (all_l2.empty()) {
            LOG_WARN("replay: no L2 snapshots for dataset '{}'; tick triggers "
                     "will not fire", dataset);
        }
        mokuren::VectorL2Feed book(all_l2);
        return Run(all_trades, book, winning_config, dataset);
    }

    // Book source supplied by the caller. Both overloads above funnel here so
    // the loop body exists once and the vector / streaming paths cannot drift
    // about when the book is observed or how the session is closed.
    //
    // `dataset` is used for logging only; it must name the file the trades and
    // the book actually came from, so a replay never evaluates one recording's
    // prints against another recording's book.
    static ReplayResult Run(const std::vector<TradeEvent>& all_trades,
                            mokuren::L2Feed& book,
                            const nlohmann::json& winning_config,
                            const std::string& dataset) {
        ReplayResult result;
        if (all_trades.empty()) return result;

        // replay is a backtest: no broker, no signed requests.
        AppConfig cfg = AppConfig::Load(winning_config, false);
        // the scan/wfa paths overwrite the resolved risk with ResolveRiskPcts
        // before building, and its legacy precedence (sl_price_pct first)
        // differs from AppConfig::Load's (sl_pct first) when a config carries
        // both keys. resolve the same way here, so a replay reproduces the run
        // that produced the config instead of a differently priced one.
        const mokuren::RiskPcts replay_risk =
            mokuren::ResolveRiskPcts(winning_config);
        cfg.sl_pct = replay_risk.sl;
        cfg.tp_pct = replay_risk.tp;

        // Build is the single arming site for trade management now: it
        // disables the PaperTrader ladder and configures the PositionManager
        // from the r-multiples in trade_management, honouring
        // break_even.enabled. re-arming the ladder here duplicated that and
        // ignored the flag, so a replay could still move a stop the backtest
        // left alone.
        EngineInstance eng = StrategyFactory::Build(cfg);

        // tick-level triggers read the book, so the replay has to replay the
        // book too. running candle-only made an orderflow strategy evaluate as
        // if every snapshot were empty, and the replay disagreed with the
        // backtest that selected the config.
        // the book arrives with the prints, from the same read of the same
        // file. resolving it separately is how a replay ended up evaluating
        // one recording's prints against another recording's book.
        // the shared execution funnel. stop validation, sizing, the risk
        // firewall, stop re-validation and take-profit derivation all live in
        // SignalExecutor, so a replay cannot drift from live or from the
        // scan/wfa paths that selected this config.
        mokuren::SignalExecutor executor(
            eng.ptrader.get(), eng.sizer.get(), eng.risk_manager.get(),
            cfg.sl_pct, cfg.tp_pct, "replay");

        // replay parity: the same guard the scan/wfa paths use, so a replay
        // reproduces the run that produced the config.
        // the same base the scan/wfa paths use, so a replay reproduces the run
        // that produced the config instead of measuring against 10000.
        const mokuren::PropRules& prop_rules = cfg.prop;
        mokuren::PropGuard prop_guard(prop_rules, cfg.initial_balance);
        // a missing prop block must not make the gate depend on ObserveProp
        // having armed the guard before the first signal.
        const bool prop_on = prop_rules.enabled;

        // The loop body lives in run_loop.cpp, shared with the grid scan and
        // both WFA sites. That keeps the three behaviours this function used
        // to own -- the `strict <` catch-up, the book tail drain, the
        // end-price fallback chain -- from drifting between callers the way
        // they did before the extraction: four of the five copies were
        // missing at least one of them, and two were missing all three.
        const mokuren::RunLoopStats loop =
            mokuren::RunLoop(eng, executor, prop_guard, prop_on, "replay",
                             all_trades, book);
        if (loop.prop_breached) {
            result.prop_breached = true;
        }

        result.history_1m = eng.aggregator->GetHistory();
        result.history_15m = eng.htf_aggregator->GetHistory();
        
        const auto& trade_log = eng.ptrader->GetTradeHistory();
        for (const auto& t : trade_log) {
            TradeInfo ti{};
            ti.is_long = (t.direction == SignalDirection::BUY); 
            ti.entry_price = t.entry_price;
            ti.exit_price = t.exit_price;
            ti.pnl = t.net_profit;
            // the inspector draws the exit tag, the exit reason and the stop
            // path from these. leaving them unset showed every replay trade as
            // a bare entry with no exit.
            ti.exit_reason = t.exit_reason;
            
            // find entry idx
            auto it = std::lower_bound(result.history_1m.begin(), result.history_1m.end(), t.entry_time, 
                [](const Bar& b, int64_t time) { return b.timestamp_start < time; });
                
            if (it != result.history_1m.end()) {
                ti.candle_idx = std::distance(result.history_1m.begin(), it);
                if (ti.candle_idx > 0 && it->timestamp_start > t.entry_time) ti.candle_idx--; 
            } else {
                ti.candle_idx = result.history_1m.empty() ? 0 : result.history_1m.size() - 1;
            }

            // find exit idx
            auto exit_it = std::lower_bound(result.history_1m.begin(), result.history_1m.end(), t.exit_time, 
                [](const Bar& b, int64_t time) { return b.timestamp_start < time; });
                
            if (exit_it != result.history_1m.end()) {
                ti.exit_candle_idx = std::distance(result.history_1m.begin(), exit_it);
                if (ti.exit_candle_idx > 0 && exit_it->timestamp_start > t.exit_time) ti.exit_candle_idx--; 
            } else {
                ti.exit_candle_idx = result.history_1m.empty() ? 0 : result.history_1m.size() - 1;
            }

            ti.stop_history = t.stop_events;
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

        // ui hygiene, inherited from Mokuren::ReplaySingleRun, which builds on
        // this function: a stop event or exit that resolves past the last bar,
        // or before the entry, would index the inspector outside the trade or
        // off the end of the chart.
        for (auto& ti : result.trades) {
            if (result.history_1m.empty()) {
                ti.candle_idx = 0;
                ti.exit_candle_idx = 0;
            } else {
                const size_t max_idx = result.history_1m.size() - 1;
                if (ti.candle_idx > max_idx) ti.candle_idx = max_idx;
                if (ti.exit_candle_idx > max_idx) ti.exit_candle_idx = max_idx;
                if (ti.exit_candle_idx < ti.candle_idx) {
                    ti.exit_candle_idx = ti.candle_idx;
                }
                for (auto& se : ti.stop_history) {
                    if (se.candle_idx > max_idx) se.candle_idx = max_idx;
                }
            }
        }
        
        return result;
    }
};
