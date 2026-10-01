#pragma once
#include "ui_pages.hpp"
#include "ui_theme.hpp"
#include "aggregator.hpp"
#include "engine_state.hpp"
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

class UIInspector : public UIPage {
private:
    std::vector<Bar> history_1m;
    std::vector<Bar> history_15m;
    std::vector<TradeInfo> trades;

    size_t current_idx = 0;
    bool use_15min_tf = false;
    
    // --- NEU: State für den Chart Toggle ---
    bool view_footprint = false; 
    // ---------------------------------------

    const std::vector<Bar>& GetActiveHistory() const { return use_15min_tf ? history_15m : history_1m; }
    double GetLevelDelta(const PriceLevel& lvl) const { return lvl.ask_volume - lvl.bid_volume; }

public:
    UIInspector() {}
    
    void SetReplayData(const std::vector<Bar>& m1, const std::vector<Bar>& m15, const std::vector<TradeInfo>& t_list) {
        history_1m = m1; history_15m = m15; trades = t_list; current_idx = 0;
    }

    void JumpToCandle(size_t target_idx) {
        if (target_idx < GetActiveHistory().size()) {
            current_idx = target_idx;
        }
    }

    void Render(struct ncplane* stdplane) override {
        unsigned int dimy, dimx;
        ncplane_dim_yx(stdplane, &dimy, &dimx);

        const auto& history = GetActiveHistory();
        if (history.empty()) {
            UITheme::StyleAlert(stdplane);
            ncplane_putstr_yx(stdplane, 11, 3, "NO REPLAY DATA. RUN BACKTEST FIRST.");
            return;
        }

        if (current_idx >= history.size()) current_idx = history.size() - 1;
        const auto& bar = history[current_idx];

        // --- GLOBAL HEADER ---
        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, 11, 3, view_footprint ? "[ ORDERFLOW FOOTPRINT ]" : "[ MACRO PRICE CHART ]"); 
        
        UITheme::StyleTextMuted(stdplane);
        std::string tf_str = use_15min_tf ? "TF: 15-MIN" : "TF: 1-MIN";
        ncplane_putstr_yx(stdplane, 11, 32, tf_str.c_str());

        char progress_buf[64];
        snprintf(progress_buf, sizeof(progress_buf), "CANDLE: %zu / %zu", current_idx + 1, history.size());
        ncplane_putstr_yx(stdplane, 11, 50, progress_buf);

        // aktiven Trade suchen
        const TradeInfo* active_trade = nullptr;
        for (const auto& t : trades) {
            if (current_idx >= t.candle_idx && current_idx <= t.exit_candle_idx) {
                active_trade = &t;
                break;
            }
        }

        // ==========================================
        // ANSICHT 1: FOOTPRINT LADDER
        // ==========================================
        if (view_footprint) {
            int stat_x = 3;
            int stat_y = 13;

            UITheme::StyleTextMuted(stdplane);
            ncplane_putstr_yx(stdplane, stat_y++, stat_x, "--- METRICS ---");

            std::time_t ts = bar.timestamp_start / 1000;
            std::tm* tm = std::localtime(&ts);
            char time_str[64];
            std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);
            UITheme::StyleTextMuted(stdplane);
            ncplane_putstr_yx(stdplane, stat_y, stat_x, "TIME  :");
            UITheme::StyleTextDefault(stdplane);
            ncplane_putstr_yx(stdplane, stat_y++, stat_x + 9, time_str);
            
            auto print_stat = [&](const char* label, double val, bool is_delta = false) {
                UITheme::StyleTextMuted(stdplane);
                ncplane_putstr_yx(stdplane, stat_y, stat_x, label);
                if (is_delta) {
                    (val >= 0) ? UITheme::StyleDataValue(stdplane) : UITheme::StyleAlert(stdplane);              
                } else { UITheme::StyleTextDefault(stdplane); }
                char val_buf[32]; snprintf(val_buf, sizeof(val_buf), "%.2f", val);
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
            
            if (active_trade) {
                UITheme::StyleTextMuted(stdplane);
                ncplane_putstr_yx(stdplane, stat_y, stat_x, "DIR   :");
                UITheme::StyleDataValue(stdplane);
                ncplane_putstr_yx(stdplane, stat_y++, stat_x + 9, active_trade->is_long ? "LONG" : "SHORT");

                print_stat("ENTRY :", active_trade->entry_price);

                if (current_idx == active_trade->exit_candle_idx) {
                    print_stat("EXIT  :", active_trade->exit_price);
                    print_stat("PnL   :", active_trade->pnl, true);
                    
                    UITheme::StyleAlert(stdplane);
                    ncplane_putstr_yx(stdplane, stat_y++, stat_x, ("[" + active_trade->exit_reason + "]").c_str());
                } else {
                    double current_sl = 0.0;
                    std::string sl_type = "INITIAL";
                    for (const auto& se : active_trade->stop_history) {
                        if (se.candle_idx <= current_idx) {
                            current_sl = se.sl_price;
                            sl_type = se.type;
                        }
                    }
                    
                    if (current_sl > 0.0) {
                        UITheme::StyleTextMuted(stdplane);
                        ncplane_putstr_yx(stdplane, stat_y, stat_x, "CUR SL:");
                        UITheme::StyleAlert(stdplane);
                        char sl_buf[32]; snprintf(sl_buf, sizeof(sl_buf), "%.2f", current_sl);
                        ncplane_putstr_yx(stdplane, stat_y++, stat_x + 9, sl_buf);
                        
                        UITheme::StyleTextMuted(stdplane);
                        ncplane_putstr_yx(stdplane, stat_y++, stat_x, ("(" + sl_type + ")").c_str());
                    } else {
                        UITheme::StyleTextMuted(stdplane);
                        ncplane_putstr_yx(stdplane, stat_y++, stat_x, "CUR SL: N/A");
                    }
                }
            } else {
                UITheme::StyleTextMuted(stdplane);
                ncplane_putstr_yx(stdplane, stat_y++, stat_x, "NO ACTIVE TRADE");
            }

            int chart_x = 32;
            int chart_y = 13;
            double max_lvl_vol = 0.0;
            for (const auto& lvl : bar.footprint) { max_lvl_vol = std::max(max_lvl_vol, lvl.bid_volume + lvl.ask_volume); }

            UITheme::StyleTextMuted(stdplane);
            ncplane_putstr_yx(stdplane, chart_y, chart_x, "   PRICE       BID | ASK       DELTA    VOLUME");
            ncplane_putstr_yx(stdplane, chart_y + 1, chart_x, "--------------------------------------------------------");

            int row = 0;
            for (auto it = bar.footprint.rbegin(); it != bar.footprint.rend(); ++it) {
                int current_y = chart_y + 2 + row;
                if (current_y >= (int)dimy - 3) break; 

                double price = it->price;
                double bid = it->bid_volume;
                double ask = it->ask_volume;
                double delta = GetLevelDelta(*it);
                
                bool bid_imbalance = (bid >= ask * 3.0 && bid > 0.0001);
                bool ask_imbalance = (ask >= bid * 3.0 && ask > 0.0001);

                if (active_trade) {
                    if (active_trade->candle_idx == current_idx && std::abs(price - active_trade->entry_price) < 0.01) {
                        active_trade->is_long ? UITheme::StyleDataValue(stdplane) : UITheme::StyleAlert(stdplane);
                        ncplane_putstr_yx(stdplane, current_y, chart_x - 4, "EN>");
                    }
                    if (active_trade->exit_candle_idx == current_idx && std::abs(price - active_trade->exit_price) < 0.01) {
                        active_trade->is_long ? UITheme::StyleDataValue(stdplane) : UITheme::StyleAlert(stdplane);
                        ncplane_putstr_yx(stdplane, current_y, chart_x + 52, "<EX");
                    }
                    for (const auto& se : active_trade->stop_history) {
                        if (se.candle_idx == current_idx && std::abs(price - se.sl_price) < 0.01) {
                            UITheme::StyleAlert(stdplane);
                            ncplane_putstr_yx(stdplane, current_y, chart_x - 4, "SL>");
                        }
                    }
                }

                char buf[64];
                (std::abs(price - bar.poc_price) < 0.0001) ? UITheme::StylePOC(stdplane) : UITheme::StyleTextDefault(stdplane);
                snprintf(buf, sizeof(buf), "%8.2f", price);
                ncplane_putstr_yx(stdplane, current_y, chart_x, buf);

                (bid_imbalance) ? UITheme::StyleBidImbalance(stdplane) : UITheme::StyleBid(stdplane);
                snprintf(buf, sizeof(buf), "%8.3f", bid);
                ncplane_putstr_yx(stdplane, current_y, chart_x + 9, buf);

                UITheme::StyleBackground(stdplane); UITheme::StyleTextMuted(stdplane);
                ncplane_putstr_yx(stdplane, current_y, chart_x + 17, " | ");

                (ask_imbalance) ? UITheme::StyleAskImbalance(stdplane) : UITheme::StyleAsk(stdplane);
                snprintf(buf, sizeof(buf), "%-8.3f", ask);
                ncplane_putstr_yx(stdplane, current_y, chart_x + 20, buf);

                UITheme::StyleBackground(stdplane);
                if (delta > 0) UITheme::StyleDataValue(stdplane);
                else if (delta < 0) UITheme::StyleAlert(stdplane);
                else UITheme::StyleTextMuted(stdplane);
                snprintf(buf, sizeof(buf), "[%+8.3f]", delta);
                ncplane_putstr_yx(stdplane, current_y, chart_x + 29, buf);           
                
                UITheme::StyleVolumeBar(stdplane);
                int bar_len = (max_lvl_vol > 0) ? std::round(((bid + ask) / max_lvl_vol) * 12) : 0;
                std::string vol_bar = ""; for(int i=0; i<bar_len; ++i) vol_bar += "█";
                ncplane_putstr_yx(stdplane, current_y, chart_x + 39, vol_bar.c_str());

                row++;
            }
        } 
        // ==========================================
        // ANSICHT 2: 2D MACRO PRICE CHART
        // ==========================================
        else {
            // horizontale metrics bar
            std::time_t ts = bar.timestamp_start / 1000;
            std::tm* tm = std::localtime(&ts);
            char info_buf[256];
            std::strftime(info_buf, sizeof(info_buf), "[%H:%M]", tm);
            
            UITheme::StyleTextMuted(stdplane);
            ncplane_putstr_yx(stdplane, 13, 3, info_buf);
            
            snprintf(info_buf, sizeof(info_buf), "O: %.2f  H: %.2f  L: %.2f  C: %.2f  V: %.1f  D: %+.1f", 
                     bar.open, bar.high, bar.low, bar.close, bar.total_volume, bar.cumulative_delta);
            UITheme::StyleTextDefault(stdplane);
            ncplane_putstr_yx(stdplane, 13, 12, info_buf);

            if (active_trade) {
                snprintf(info_buf, sizeof(info_buf), " | TRADE: %s | EN: %.2f", 
                         active_trade->is_long ? "LONG" : "SHORT", active_trade->entry_price);
                UITheme::StyleDataValue(stdplane);
                ncplane_putstr_yx(stdplane, 13, 75, info_buf);
            }

            int chart_y_start = 15;
            int chart_h = (dimy - 4) - chart_y_start; 
            int chart_w = dimx - 16; 

            if (chart_h > 0 && chart_w > 0) {
                int max_candles = chart_w;
                int start_idx = std::max(0, (int)current_idx - max_candles / 2);
                int end_idx = std::min((int)history.size() - 1, start_idx + max_candles - 1);
                int visible_count = end_idx - start_idx + 1;
                
                double min_p = history[start_idx].low;
                double max_p = history[start_idx].high;
                for (int i = 0; i < visible_count; i++) {
                    min_p = std::min(min_p, history[start_idx + i].low);
                    max_p = std::max(max_p, history[start_idx + i].high);
                }
                if (max_p == min_p) { max_p += 1; min_p -= 1; }
                double price_step = (max_p - min_p) / chart_h;
                
                // Y-Axis rendern
                UITheme::StyleTextMuted(stdplane);
                for (int y = 0; y < chart_h; y++) {
                    double p = max_p - (y * price_step);
                    char p_buf[16]; snprintf(p_buf, sizeof(p_buf), "%8.2f", p);
                    ncplane_putstr_yx(stdplane, chart_y_start + y, chart_w + 5, p_buf);
                }
                
                // OHLC Kerzen rendern
                for (int i = 0; i < visible_count; i++) {
                    int c_idx = start_idx + i;
                    const auto& b = history[c_idx];
                    int x = 3 + i;
                    
                    bool is_up = b.close >= b.open;
                    
                    int y_high = chart_y_start + chart_h - 1 - std::clamp((int)((b.high - min_p) / price_step), 0, chart_h - 1);
                    int y_low  = chart_y_start + chart_h - 1 - std::clamp((int)((b.low - min_p) / price_step), 0, chart_h - 1);
                    int y_open = chart_y_start + chart_h - 1 - std::clamp((int)((b.open - min_p) / price_step), 0, chart_h - 1);
                    int y_close= chart_y_start + chart_h - 1 - std::clamp((int)((b.close - min_p) / price_step), 0, chart_h - 1);
                    
                    int y_top = std::min(y_open, y_close);
                    int y_bot = std::max(y_open, y_close);
                    
                    if (is_up) UITheme::StyleCandleUp(stdplane);
                    else UITheme::StyleCandleDown(stdplane);
                    
                    for (int y = y_high; y <= y_low; y++) {
                        if (y >= y_top && y <= y_bot) ncplane_putstr_yx(stdplane, y, x, "█");
                        else ncplane_putstr_yx(stdplane, y, x, "│");
                    }
                    
                    // aktiven Cursor markieren
                    if (c_idx == current_idx) {
                        UITheme::StyleCursorActive(stdplane);
                        ncplane_putstr_yx(stdplane, chart_y_start + chart_h, x, "^");
                    }
                    
                    // Trade Entry/Exit Markierungen in den Chart zeichnen
                    for (const auto& t : trades) {
                        if (t.candle_idx == c_idx) {
                            if (t.is_long) { UITheme::StyleDataValue(stdplane); ncplane_putstr_yx(stdplane, y_low + 1, x, "L"); }
                            else { UITheme::StyleAlert(stdplane); ncplane_putstr_yx(stdplane, y_high - 1, x, "S"); }
                        }
                        if (t.exit_candle_idx == c_idx) {
                            UITheme::StyleTextMuted(stdplane);
                            ncplane_putstr_yx(stdplane, t.is_long ? y_high - 1 : y_low + 1, x, "x");
                        }
                    }
                }
            }
        }

        // --- GLOBAL FOOTER ---
        std::string hline = std::string(dimx > 6 ? dimx - 6 : 10, '=');
        UITheme::StyleBackground(stdplane);
        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, dimy - 3, 3, hline.c_str());
        
        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, dimy - 2, 3, "[<- / ->] NAVIGATE");
        ncplane_putstr_yx(stdplane, dimy - 2, 25, "[T] TIMEFRAME");
        ncplane_putstr_yx(stdplane, dimy - 2, 42, "[n / N] JUMP TO TRADE");
        
        UITheme::StyleAlert(stdplane);
        ncplane_putstr_yx(stdplane, dimy - 2, dimx - 30, "[ENTER] TOGGLE CHART/FOOTPRINT");
    }

    void HandleInput(uint32_t key) override {
        const auto& history = GetActiveHistory();
        if (history.empty()) return;

        if (key == NCKEY_LEFT && current_idx > 0) current_idx--;
        else if (key == NCKEY_RIGHT && current_idx < history.size() - 1) current_idx++;
        // --- NEU: Toggle zwischen Makro-Chart und Footprint ---
        else if (key == NCKEY_ENTER) {
            view_footprint = !view_footprint;
        }
        // ------------------------------------------------------
        else if (key == 't' || key == 'T') {
            use_15min_tf = !use_15min_tf;
            if (current_idx >= GetActiveHistory().size()) {
                current_idx = GetActiveHistory().empty() ? 0 : GetActiveHistory().size() - 1;
            }
        }
        else if (key == 'n') {
            for (const auto& t : trades) {
                if (t.candle_idx > current_idx) { current_idx = t.candle_idx; break; }
            }
        }
        else if (key == 'N') {
            for (auto it = trades.rbegin(); it != trades.rend(); ++it) {
                if (it->candle_idx < current_idx) { current_idx = it->candle_idx; break; }
            }
        }
    }
};