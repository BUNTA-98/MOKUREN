#pragma once
#include "ui_pages.hpp"
#include "ui_theme.hpp"
#include "engine_state.hpp" 
#include <vector>
#include <string>
#include <functional>
#include <ctime>
#include <algorithm>

class UITradelog : public UIPage {
private:
    std::vector<TradeInfo> trades;
    std::vector<Bar> history;
    int selected_idx = 0;
    int scroll_offset = 0;
    bool show_popup = false; // toggle for trailing details
    std::function<void(size_t)> on_inspect_trade;

    // convert unix ms to clean readable date format
    std::string FormatTime(int64_t ts) {
        if (ts == 0) return "UNKNOWN";
        time_t t = ts / 1000;
        struct tm* tm_info = localtime(&t);
        char buf[32];
        strftime(buf, sizeof(buf), "%m-%d %H:%M", tm_info);
        return std::string(buf);
    }

public:
    UITradelog(std::function<void(size_t)> inspect_cb) : on_inspect_trade(inspect_cb) {}

    void SetData(const std::vector<TradeInfo>& t, const std::vector<Bar>& h) {
        trades = t;
        history = h;
        selected_idx = 0;
        scroll_offset = 0;
        show_popup = false;
    }

    void Render(struct ncplane* stdplane) override {
        unsigned int dimy, dimx;
        ncplane_dim_yx(stdplane, &dimy, &dimx);

        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, 10, 3, "[ TRADE JOURNAL ]");

        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 12, 3, " ID   DIR    ENTRY TIME      ENTRY $      EXIT TIME       EXIT $       MOVE %     PROFIT $   REASON");
        std::string hline = std::string(dimx > 6 ? dimx - 6 : 10, '-');
        ncplane_putstr_yx(stdplane, 13, 3, hline.c_str());

        if (trades.empty()) {
            UITheme::StyleAlert(stdplane);
            ncplane_putstr_yx(stdplane, 15, 3, "ERR: NO TRADES EXECUTED IN THIS RUN.");
            return;
        }

        int limit = trades.size();
        int max_visible = std::max(1, (int)dimy - 17);
        int scroll_margin = 2;

        if (selected_idx < scroll_offset + scroll_margin) {
            scroll_offset = std::max(0, selected_idx - scroll_margin);
        } else if (selected_idx >= scroll_offset + max_visible - scroll_margin) {
            scroll_offset = std::min(std::max(0, limit - max_visible), selected_idx - max_visible + scroll_margin + 1);
        }

        for (int i = 0; i < max_visible && (scroll_offset + i) < limit; i++) {
            int idx = scroll_offset + i;
            const auto& t = trades[idx];
            bool is_active = (idx == selected_idx);

            if (is_active) {
                UITheme::StyleCursorActive(stdplane);
                ncplane_putstr_yx(stdplane, 14 + i, 1, ">");
            } else {
                ncplane_putstr_yx(stdplane, 14 + i, 1, " ");
            }

            std::string dir = t.is_long ? "LONG " : "SHORT";
            int64_t en_ts = (history.size() > t.candle_idx) ? history[t.candle_idx].timestamp_start : 0;
            int64_t ex_ts = (history.size() > t.exit_candle_idx) ? history[t.exit_candle_idx].timestamp_start : 0;
            
            double move_pct = (t.entry_price > 0) ? ((t.exit_price - t.entry_price) / t.entry_price) * 100.0 : 0.0;
            if (!t.is_long) move_pct = -move_pct;

            char buf[256];
            snprintf(buf, sizeof(buf), " %-4d %-6s %-15s %-12.2f %-15s %-12.2f %-+8.2f%% %-+10.2f %-8s",
                     idx + 1, dir.c_str(), FormatTime(en_ts).c_str(), t.entry_price,
                     FormatTime(ex_ts).c_str(), t.exit_price, move_pct, t.pnl, 
                     t.exit_reason.empty() ? "N/A" : t.exit_reason.c_str()); 

            if (is_active) {
                UITheme::StyleCursorActive(stdplane);
            } else if (t.pnl > 0) {
                UITheme::StyleDataValue(stdplane); 
            } else if (t.pnl < 0) {
                UITheme::StyleAlert(stdplane);
            } else {
                UITheme::StyleTextDefault(stdplane);
            }
            
            ncplane_putstr_yx(stdplane, 14 + i, 2, buf);
        }

        UITheme::StyleAlert(stdplane); 
        ncplane_putstr_yx(stdplane, dimy - 1, 3, "[UP/DOWN] SELECT   [ENTER] VIEW IN FOOTPRINT   [D] SL/TRAILING TIMELINE");

        // render trailing history popup overlay
        if (show_popup && !trades.empty() && selected_idx < trades.size()) {
            int p_w = 60;
            int p_h = 16;
            int p_y = (dimy - p_h) / 2;
            int p_x = (dimx - p_w) / 2;

            UITheme::StyleBackground(stdplane);
            for(int i = 0; i < p_h; i++) {
                 ncplane_putstr_yx(stdplane, p_y + i, p_x, std::string(p_w, ' ').c_str());
            }
            
            UITheme::StyleDataValue(stdplane);
            ncplane_putstr_yx(stdplane, p_y, p_x, std::string(p_w, '=').c_str());
            ncplane_putstr_yx(stdplane, p_y + p_h - 1, p_x, std::string(p_w, '=').c_str());
            for(int i = 1; i < p_h - 1; i++) {
                ncplane_putstr_yx(stdplane, p_y + i, p_x, "|");
                ncplane_putstr_yx(stdplane, p_y + i, p_x + p_w - 1, "|");
            }

            UITheme::StyleTextDefault(stdplane);
            ncplane_putstr_yx(stdplane, p_y + 1, p_x + 2, "[ TRADE SL/TRAILING TIMELINE ]");
            
            const auto& t = trades[selected_idx];
            if (t.stop_history.empty()) {
                UITheme::StyleTextMuted(stdplane);
                ncplane_putstr_yx(stdplane, p_y + 3, p_x + 4, "NO TRAILING OR BREAK-EVEN EVENTS RECORDED.");
            } else {
                UITheme::StyleTextMuted(stdplane);
                int line_y = p_y + 3;
                
                // render last N events fitting the box
                int max_events = p_h - 6;
                int start_ev = std::max(0, (int)t.stop_history.size() - max_events);
                
                for(size_t e = start_ev; e < t.stop_history.size(); e++) {
                    const auto& ev = t.stop_history[e];
                    char tbuf[128];
                    snprintf(tbuf, sizeof(tbuf), "[%s] %-12s @ $%.2f", 
                             FormatTime(ev.timestamp).c_str(), ev.type.c_str(), ev.sl_price);
                    ncplane_putstr_yx(stdplane, line_y++, p_x + 4, tbuf);
                }
            }

            UITheme::StyleAlert(stdplane);
            ncplane_putstr_yx(stdplane, p_y + p_h - 2, p_x + 2, "[ANY KEY] CLOSE TIMELINE");
        }
    }

    void HandleInput(uint32_t key) override {
        if (show_popup) {
            show_popup = false; 
            return; 
        }

        if (key == 'd' || key == 'D') {
            show_popup = true;
            return;
        }

        if (trades.empty()) return;
        int limit = trades.size();

        if (key == NCKEY_UP && selected_idx > 0) selected_idx--;
        if (key == NCKEY_DOWN && selected_idx < limit - 1) selected_idx++;
        
        if (key == NCKEY_ENTER) {
            on_inspect_trade(trades[selected_idx].candle_idx);
        }
    }
};