#pragma once
#include "ui_pages.hpp"
#include "ui_theme.hpp"
#include "mokuren.hpp"
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdio>

class UIEquityCurve : public UIPage {
private:
    std::vector<TradeInfo> trades;
    std::vector<double> equity_curve;
    double max_dd = 0.0;
    double final_pnl = 0.0;

    void CalculateCurve() {
        equity_curve.clear();
        double current_pnl = 0.0;
        double peak = 0.0;
        max_dd = 0.0;
        
        // Startwert (0.0)
        equity_curve.push_back(current_pnl);

        for (const auto& t : trades) {
            current_pnl += t.pnl;
            equity_curve.push_back(current_pnl);

            if (current_pnl > peak) peak = current_pnl;
            double dd = peak - current_pnl;
            if (dd > max_dd) max_dd = dd;
        }
        final_pnl = current_pnl;
    }

public:
    UIEquityCurve() {}

    void SetReplayData(const std::vector<TradeInfo>& t_list) {
        trades = t_list;
        CalculateCurve();
    }

    void Render(struct ncplane* stdplane) override {
        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, 7, 3, "[ MACRO VIEW : EQUITY CURVE ]");

        if (equity_curve.empty() || trades.empty()) {
            UITheme::StyleAlert(stdplane);
            ncplane_putstr_yx(stdplane, 10, 3, "NO TRADE DATA TO PLOT. RUN BACKTEST FIRST.");
            return;
        }

        // --- Metriken ---
        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 9, 3, "TOTAL TRADES :");
        UITheme::StyleDataValue(stdplane);
        ncplane_putstr_yx(stdplane, 9, 18, std::to_string(trades.size()).c_str());

        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 10, 3, "NET PNL      :");
        (final_pnl >= 0) ? UITheme::StyleDataValue(stdplane) : UITheme::StyleAlert(stdplane);
        char pnl_buf[32]; snprintf(pnl_buf, sizeof(pnl_buf), "$%.2f", final_pnl);
        ncplane_putstr_yx(stdplane, 10, 18, pnl_buf);

        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 11, 3, "MAX DRAWDOWN :");
        UITheme::StyleAlert(stdplane);
        char dd_buf[32]; snprintf(dd_buf, sizeof(dd_buf), "$%.2f", max_dd);
        ncplane_putstr_yx(stdplane, 11, 18, dd_buf);

        // --- Custom Block Plotter (Simpel & TUI-freundlich) ---
        int chart_x = 35;
        int chart_y = 8;
        int chart_width = 50;
        int chart_height = 10;

        double min_val = *std::min_element(equity_curve.begin(), equity_curve.end());
        double max_val = *std::max_element(equity_curve.begin(), equity_curve.end());
        double range = max_val - min_val;
        if (range == 0) range = 1.0; 

        // Y-Achsen Labels
        UITheme::StyleTextMuted(stdplane);
        char label_buf[32];
        snprintf(label_buf, sizeof(label_buf), "$%.0f -", max_val);
        ncplane_putstr_yx(stdplane, chart_y, chart_x - 8, label_buf);
        snprintf(label_buf, sizeof(label_buf), "$%.0f -", min_val);
        ncplane_putstr_yx(stdplane, chart_y + chart_height - 1, chart_x - 8, label_buf);

        // Graph zeichnen
        for (int i = 0; i < chart_width; ++i) {
            int data_idx = (i * equity_curve.size()) / chart_width;
            if (data_idx >= equity_curve.size()) data_idx = equity_curve.size() - 1;
            
            double val = equity_curve[data_idx];
            
            int y_pos = chart_height - 1 - static_cast<int>(((val - min_val) / range) * (chart_height - 1));
            
            for (int y = 0; y < chart_height; ++y) {
                if (y == y_pos) {
                    (val >= 0) ? UITheme::StyleDataValue(stdplane) : UITheme::StyleAlert(stdplane);
                    ncplane_putstr_yx(stdplane, chart_y + y, chart_x + i, "█");
                } else if (y > y_pos && val >= 0) {
                    UITheme::StyleVolumeBar(stdplane);
                    ncplane_putstr_yx(stdplane, chart_y + y, chart_x + i, "│");
                } else if (y < y_pos && val < 0) {
                    UITheme::StyleAlert(stdplane);
                    ncplane_putstr_yx(stdplane, chart_y + y, chart_x + i, "│");
                }
            }
        }

        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 26, 3, "========================================================================================");
    }

    void HandleInput(uint32_t key) override {
        // Navigation (TAB etc.) wird komplett vom UIManager gesteuert
    }
};