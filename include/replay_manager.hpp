#pragma once
#include "data_manager.hpp"
#include "factory.hpp"
#include "logger.hpp"
#include "market_context.hpp"
#include "trade_event.hpp"
#include "trade_management.hpp"
#include "ui_footprint.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

struct ReplayResult {
    std::vector<Bar> history_1m;
    std::vector<Bar> history_15m;
    std::vector<TradeInfo> trades; 
};

class ReplayManager {
public:
    static ReplayResult Run(const std::vector<TradeEvent>& all_trades, const nlohmann::json& winning_config) {
        ReplayResult result;
        if (all_trades.empty()) return result;

        AppConfig cfg = AppConfig::Load(winning_config);
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
        const std::string dataset =
            winning_config.value("environment", nlohmann::json::object())
                .value("dataset", "data");
        const std::vector<L2Snapshot> all_l2 =
            DataManager::LoadAllL2Snapshots(dataset);
        if (all_l2.empty()) {
            LOG_WARN("replay: no L2 snapshots for dataset '{}'; tick triggers "
                     "will not fire", dataset);
        }

        // one funnel for tick and candle signals, in the same order as the
        // backtest: validate the placed stop, size, run the risk firewall, and
        // only then derive the take profit from the stop that survived.
        auto execute_signal = [&](TradeSignal raw_signal, double exec_price,
                                  int64_t ts) {
            if (raw_signal.direction == SignalDirection::NONE) return;

            const bool is_long = raw_signal.direction == SignalDirection::BUY;
            if (!(raw_signal.stop_loss > 0.0) ||
                (is_long && raw_signal.stop_loss >= exec_price) ||
                (!is_long && raw_signal.stop_loss <= exec_price)) {
                LOG_ERROR("replay: dropping {} signal at {}: stop_loss={} is "
                          "missing or on the wrong side of entry",
                          is_long ? "long" : "short", exec_price,
                          raw_signal.stop_loss);
                return;
            }
            raw_signal.entry_price = exec_price;

            TradeSignal sized_signal = eng.sizer->CalculateSize(raw_signal);
            TradeSignal final_signal =
                eng.risk_manager->Evaluate(sized_signal, ts);
            if (final_signal.direction == SignalDirection::NONE) return;

            // cfg.sl_pct / cfg.tp_pct already come from the risk block:
            // AppConfig::Load resolves tp_r against sl_pct, so dividing
            // recovers the r the author wrote.
            if (!(final_signal.take_profit > 0.0)) {
                if (!(cfg.sl_pct > 0.0)) {
                    LOG_ERROR("replay: cannot derive take profit from tp_pct={} "
                              "with sl_pct={}", cfg.tp_pct, cfg.sl_pct);
                    return;
                }
                const bool final_long =
                    final_signal.direction == SignalDirection::BUY;
                const double risk_pct =
                    std::abs(exec_price - final_signal.stop_loss) / exec_price;
                const double tp_pct = (cfg.tp_pct / cfg.sl_pct) * risk_pct;
                final_signal.take_profit =
                    final_long ? exec_price * (1.0 + tp_pct)
                               : exec_price * (1.0 - tp_pct);
            }

            eng.ptrader->ProcessSignal(final_signal, exec_price, ts);
        };

        Bar live_bar;
        Bar htf_bar;

        size_t l2_idx = 0;

        for (const auto &trade : all_trades) {
            // replay the book up to this trade, then evaluate tick triggers
            // against it -- mirrors the backtest l2 catch-up loop.
            while (l2_idx < all_l2.size() &&
                   all_l2[l2_idx].timestamp <= trade.timestamp) {
                eng.aggregator->ProcessL2(all_l2[l2_idx]);
                eng.htf_aggregator->ProcessL2(all_l2[l2_idx]);

                MarketContext tick_ctx{
                    eng.aggregator->GetHistory(),
                    live_bar,
                    htf_bar,
                    eng.aggregator->GetSessionMetrics(),
                    eng.aggregator->GetLatestL2(),
                    eng.aggregator->GetL2History()
                };

                TradeSignal tick_signal = eng.alpha->Evaluate(tick_ctx, false);
                if (tick_signal.direction != SignalDirection::NONE) {
                    // cross the spread for hft execution.
                    double exec_price =
                        (tick_signal.direction == SignalDirection::BUY)
                            ? all_l2[l2_idx].best_ask_price
                            : all_l2[l2_idx].best_bid_price;
                    if (exec_price <= 0.0) {
                        exec_price = live_bar.close > 0.0 ? live_bar.close
                                                          : trade.price;
                    }
                    execute_signal(tick_signal, exec_price,
                                   all_l2[l2_idx].timestamp);
                }
                l2_idx++;
            }

            // pass ts to pos manager
            eng.pos_manager->Update(trade.price, trade.timestamp);
            eng.ptrader->CheckRisk(trade.price, trade.timestamp);

            bool candle_finished = eng.aggregator->ProcessTrade(live_bar, trade);
            eng.htf_aggregator->ProcessTrade(htf_bar, trade);
            
            // build market context
            MarketContext context{
                eng.aggregator->GetHistory(), 
                live_bar, 
                htf_bar, 
                eng.aggregator->GetSessionMetrics(), 
                eng.aggregator->GetLatestL2(), 
                eng.aggregator->GetL2History()
            };

            TradeSignal candle_signal = eng.alpha->Evaluate(context, candle_finished);
            execute_signal(candle_signal, trade.price, trade.timestamp);
        }

        eng.aggregator->FlushLastCandle(live_bar);
        eng.ptrader->CloseOpenPositionAtEnd(live_bar.close, live_bar.timestamp_start);

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
