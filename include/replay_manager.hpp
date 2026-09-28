#pragma once
#include "factory.hpp"
#include "market_context.hpp"
#include "ui_footprint.hpp"
#include <vector>
#include <algorithm>
#include <nlohmann/json.hpp>

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
            ti.exit_price = t.exit_price;
            ti.pnl = t.net_profit;
            
            // Finde Entry Candle
            auto it = std::lower_bound(result.history_1m.begin(), result.history_1m.end(), t.entry_time, 
                [](const Bar& b, int64_t time) { return b.timestamp_start < time; });
                
            if (it != result.history_1m.end()) {
                ti.candle_idx = std::distance(result.history_1m.begin(), it);
                if (ti.candle_idx > 0 && it->timestamp_start > t.entry_time) ti.candle_idx--; 
            } else {
                ti.candle_idx = result.history_1m.empty() ? 0 : result.history_1m.size() - 1;
            }

            // Finde Exit Candle
            auto exit_it = std::lower_bound(result.history_1m.begin(), result.history_1m.end(), t.exit_time, 
                [](const Bar& b, int64_t time) { return b.timestamp_start < time; });
                
            if (exit_it != result.history_1m.end()) {
                ti.exit_candle_idx = std::distance(result.history_1m.begin(), exit_it);
                if (ti.exit_candle_idx > 0 && exit_it->timestamp_start > t.exit_time) ti.exit_candle_idx--; 
            } else {
                ti.exit_candle_idx = result.history_1m.empty() ? 0 : result.history_1m.size() - 1;
            }

            result.trades.push_back(ti);
        }
        
        return result;
    }
};