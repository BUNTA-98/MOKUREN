#pragma once
#include "ui_pages.hpp"
#include "ui_theme.hpp"
#include "aggregator.hpp"
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdio>

struct TradeInfo {
    size_t candle_idx;
    bool is_long;
    double entry_price;
    double pnl;
};

class UIInspector : public UIPage {
private:
    std::vector<Bar> history_1m;
    std::vector<Bar> history_15m;
    std::vector<TradeInfo> trades;

    size_t current_idx = 0;
    bool use_15min_tf = false;

    const std::vector<Bar>& GetActiveHistory() const {
        return use_15min_tf ? history_15m : history_1m;
    }

    double GetLevelDelta(const PriceLevel& lvl) const {
        return lvl.ask_volume - lvl.bid_volume; 
    }

public:
    UIInspector() {}

    void SetReplayData(const std::vector<Bar>& m1, 
                       const std::vector<Bar>& m15, 
                       const std::vector<TradeInfo>& t_list) {
        history_1m = m1;
        history_15m = m15;
        trades = t_list;
        current_idx = 0;
    }

    void Render(struct ncplane* stdplane) override {
        const auto& history = GetActiveHistory();
        if (history.empty()) {
            UITheme::StyleAlert(stdplane);
            ncplane_putstr_yx(stdplane, 8, 3, "NO REPLAY DATA. RUN BACKTEST FIRST.");
            return;
        }

        if (current_idx >= history.size()) current_idx = history.size() - 1;
        const auto& bar = history[current_idx];

        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, 2, 3, "[ ORDERFLOW FOOTPRINT ]");
        
        UITheme::StyleTextMuted(stdplane);
        std::string tf_str = use_15min_tf ? "TF: 15-MIN" : "TF: 1-MIN";
        ncplane_putstr_yx(stdplane, 2, 40, tf_str.c_str());

        char progress_buf[64];
        snprintf(progress_buf, sizeof(progress_buf), "CANDLE: %zu / %zu", current_idx + 1, history.size());
        ncplane_putstr_yx(stdplane, 2, 60, progress_buf);

        // --- LINKE SEITE (Metriken) ---
        int stat_x = 3;
        int stat_y = 7;

        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, stat_y++, stat_x, "--- METRICS ---");
        
        auto print_stat = [&](const char* label, double val, bool is_delta = false) {
            UITheme::StyleTextMuted(stdplane);
            ncplane_putstr_yx(stdplane, stat_y, stat_x, label);
            if (is_delta) {
                if (val >= 0) UITheme::StyleDataValue(stdplane); 
                else UITheme::StyleAlert(stdplane);              
            } else {
                UITheme::StyleTextDefault(stdplane);
            }
            char val_buf[32];
            snprintf(val_buf, sizeof(val_buf), "%.2f", val);
            ncplane_putstr_yx(stdplane, stat_y++, stat_x + 9, val_buf);
        };

        print_stat("OPEN  :", bar.open);
        print_stat("HIGH  :", bar.high);
        print_stat("LOW   :", bar.low);
        print_stat("CLOSE :", bar.close);
        print_stat("VOL   :", bar.total_volume);
        print_stat("DELTA :", bar.cumulative_delta, true);

        stat_y += 2;
        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, stat_y++, stat_x, "--- TRADE ---");
        
        bool active_trade = false;
        for (const auto& t : trades) {
            if (t.candle_idx == current_idx) {
                active_trade = true;
                UITheme::StyleTextMuted(stdplane);
                ncplane_putstr_yx(stdplane, stat_y, stat_x, "DIR   :");
                UITheme::StyleDataValue(stdplane);
                ncplane_putstr_yx(stdplane, stat_y++, stat_x + 9, t.is_long ? "LONG" : "SHORT");

                print_stat("ENTRY :", t.entry_price);
                print_stat("PnL   :", t.pnl, true);
                break;
            }
        }
        
        if (!active_trade) {
            UITheme::StyleTextMuted(stdplane);
            ncplane_putstr_yx(stdplane, stat_y, stat_x, "NO TRADE");
        }

        // --- RECHTE SEITE (Die Footprint Leiter) ---
        int chart_x = 32;
        int chart_y = 7;

        // Finde das maximale Volumen für das Histogramm
        double max_lvl_vol = 0.0;
        for (const auto& lvl : bar.footprint) {
            max_lvl_vol = std::max(max_lvl_vol, lvl.bid_volume + lvl.ask_volume);
        }

        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, chart_y, chart_x, "   PRICE       BID | ASK       DELTA    VOLUME");
        ncplane_putstr_yx(stdplane, chart_y + 1, chart_x, "--------------------------------------------------------");

        int row = 0;
        for (auto it = bar.footprint.rbegin(); it != bar.footprint.rend(); ++it) {
            int current_y = chart_y + 2 + row;
            if (current_y >= 26) break; 

            double price = it->price;
            double bid = it->bid_volume;
            double ask = it->ask_volume;
            double delta = GetLevelDelta(*it);
            
            // Imbalance Check (Faktor 3x größer als Gegenseite, mind. > 0)
            bool bid_imbalance = (bid >= ask * 3.0 && bid > 0.0001);
            bool ask_imbalance = (ask >= bid * 3.0 && ask > 0.0001);

            char buf[64];

            // 1. PRICE
            if (std::abs(price - bar.poc_price) < 0.0001) UITheme::StylePOC(stdplane); 
            else UITheme::StyleTextDefault(stdplane);
            snprintf(buf, sizeof(buf), "%8.2f", price);
            ncplane_putstr_yx(stdplane, current_y, chart_x, buf);

            // 2. BID VOLUME (Rechtsbündig)
            if (bid_imbalance) UITheme::StyleBidImbalance(stdplane);
            else UITheme::StyleBid(stdplane);
            snprintf(buf, sizeof(buf), "%6.0f", bid);
            ncplane_putstr_yx(stdplane, current_y, chart_x + 10, buf);

            // Trenner
            UITheme::StyleTextMuted(stdplane);
            ncplane_putstr_yx(stdplane, current_y, chart_x + 17, " | ");

            // 3. ASK VOLUME (Linksbündig)
            if (ask_imbalance) UITheme::StyleAskImbalance(stdplane);
            else UITheme::StyleAsk(stdplane);
            snprintf(buf, sizeof(buf), "%-6.0f", ask);
            ncplane_putstr_yx(stdplane, current_y, chart_x + 20, buf);

            // 4. DELTA
            if (delta > 0) UITheme::StyleDataValue(stdplane);
            else if (delta < 0) UITheme::StyleAlert(stdplane);
            else UITheme::StyleTextMuted(stdplane);
            snprintf(buf, sizeof(buf), "[%+6.0f]", delta);
            ncplane_putstr_yx(stdplane, current_y, chart_x + 28, buf);

            // 5. VOLUME HISTOGRAM
            UITheme::StyleVolumeBar(stdplane);
            int bar_len = (max_lvl_vol > 0) ? std::round(((bid + ask) / max_lvl_vol) * 12) : 0;
            std::string vol_bar = "";
            for(int i = 0; i < bar_len; ++i) vol_bar += "█";
            ncplane_putstr_yx(stdplane, current_y, chart_x + 39, vol_bar.c_str());

            row++;
        }
        
        // --- FOOTER ---
        UITheme::StyleBackground(stdplane); // Setzt den Background für den Footer zurück
        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 28, 3, "========================================================================================");
        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, 29, 3, "[<- / ->] NAVIGATE TIME");
        ncplane_putstr_yx(stdplane, 29, 30, "[T] TOGGLE TIMEFRAME");
        ncplane_putstr_yx(stdplane, 29, 55, "[N] JUMP TO TRADE");
    }

    void HandleInput(uint32_t key) override {
        const auto& history = GetActiveHistory();
        if (history.empty()) return;

        if (key == NCKEY_LEFT && current_idx > 0) current_idx--;
        else if (key == NCKEY_RIGHT && current_idx < history.size() - 1) current_idx++;
        else if (key == 't' || key == 'T') {
            use_15min_tf = !use_15min_tf;
            if (current_idx >= GetActiveHistory().size()) {
                current_idx = GetActiveHistory().empty() ? 0 : GetActiveHistory().size() - 1;
            }
        }
        else if (key == 'n' || key == 'N') {
            for (const auto& t : trades) {
                if (t.candle_idx > current_idx) {
                    current_idx = t.candle_idx;
                    break;
                }
            }
        }
    }
};
