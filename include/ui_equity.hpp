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
    double max_dd_pct = 0.0;
    double final_pnl = 0.0;

    void CalculateCurve() {
        equity_curve.clear();
        double current_pnl = 0.0;
        
        // identische drawdown-logik zum papertrader
        double current_equity = 10000.0;
        double peak_equity = 10000.0;
        max_dd_pct = 0.0;
        
        equity_curve.push_back(current_pnl);

        for (const auto& t : trades) {
            current_pnl += t.pnl;
            current_equity = 10000.0 + current_pnl;
            equity_curve.push_back(current_pnl);
            
            if (current_equity > peak_equity) {
                peak_equity = current_equity;
            }
            
            double dd = ((peak_equity - current_equity) / peak_equity) * 100.0;
            if (dd > max_dd_pct) {
                max_dd_pct = dd;
            }
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
        unsigned int dimy, dimx;
        ncplane_dim_yx(stdplane, &dimy, &dimx);

        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, 11, 3, "[ MACRO VIEW : EQUITY CURVE ]");

        if (equity_curve.empty() || trades.empty()) {
            UITheme::StyleAlert(stdplane);
            // warnung, falls die datei aus dem UIManager nicht gefunden wurde
            ncplane_putstr_yx(stdplane, 13, 3, "NO TRADE DATA. (CHECK CSV DATAPATH IN UI_MANAGER!)");
            return;
        }

        // --- metrics block ---
        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 13, 3, "TOTAL TRADES :");
        UITheme::StyleDataValue(stdplane);
        ncplane_putstr_yx(stdplane, 13, 18, std::to_string(trades.size()).c_str());

        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 14, 3, "NET PNL      :");
        (final_pnl >= 0) ? UITheme::StyleDataValue(stdplane) : UITheme::StyleAlert(stdplane);
        char pnl_buf[32]; snprintf(pnl_buf, sizeof(pnl_buf), "$%.2f", final_pnl);
        ncplane_putstr_yx(stdplane, 14, 18, pnl_buf);

        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 15, 3, "MAX DRAWDOWN :");
        UITheme::StyleAlert(stdplane);
        // anzeige auf prozent korrigiert
        char dd_buf[32]; snprintf(dd_buf, sizeof(dd_buf), "-%.2f%%", max_dd_pct);
        ncplane_putstr_yx(stdplane, 15, 18, dd_buf);

        // --- responsive braille plotter ---
        int chart_x = 35;
        int chart_y = 12;
        
        int chart_width = std::max(10, (int)dimx - chart_x - 5);
        int chart_height = std::max(5, (int)dimy - chart_y - 3);

        double min_val = *std::min_element(equity_curve.begin(), equity_curve.end());
        double max_val = *std::max_element(equity_curve.begin(), equity_curve.end());
        double range = max_val - min_val;
        if (range == 0) range = 1.0; 

        UITheme::StyleTextMuted(stdplane);
        char label_buf[32];
        snprintf(label_buf, sizeof(label_buf), "$%.0f -", max_val);
        ncplane_putstr_yx(stdplane, chart_y, chart_x - 8, label_buf);
        snprintf(label_buf, sizeof(label_buf), "$%.0f -", min_val);
        ncplane_putstr_yx(stdplane, chart_y + chart_height - 1, chart_x - 8, label_buf);

        int pixel_width = chart_width * 2;
        int pixel_height = chart_height * 4;
        std::vector<std::vector<uint8_t>> braille_grid(chart_width, std::vector<uint8_t>(chart_height, 0));

        for (int px = 0; px < pixel_width; ++px) {
            int data_idx = (px * equity_curve.size()) / pixel_width;
            if (data_idx >= equity_curve.size()) data_idx = equity_curve.size() - 1;
            
            double val = equity_curve[data_idx];
            int py = pixel_height - 1 - static_cast<int>(((val - min_val) / range) * (pixel_height - 1));
            
            if (py < 0) py = 0;
            if (py >= pixel_height) py = pixel_height - 1;
            
            int char_x = px / 2;
            int char_y = py / 4;
            int dot_x = px % 2;
            int dot_y = py % 4;
            
            static const uint8_t braille_dots[2][4] = {
                {0x1, 0x2, 0x4, 0x40},
                {0x8, 0x10, 0x20, 0x80}
            };
            braille_grid[char_x][char_y] |= braille_dots[dot_x][dot_y];
        }

        UITheme::StyleDataValue(stdplane); 
        for (int cy = 0; cy < chart_height; ++cy) {
            for (int cx = 0; cx < chart_width; ++cx) {
                uint8_t v = braille_grid[cx][cy];
                if (v != 0) {
                    int code = 0x2800 + v;
                    char utf8[4];
                    utf8[0] = 0xE0 | (code >> 12);
                    utf8[1] = 0x80 | ((code >> 6) & 0x3F);
                    utf8[2] = 0x80 | (code & 0x3F);
                    utf8[3] = '\0';
                    ncplane_putstr_yx(stdplane, chart_y + cy, chart_x + cx, utf8);
                }
            }
        }

        std::string hline = std::string(dimx > 6 ? dimx - 6 : 10, '=');
        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, dimy - 2, 3, hline.c_str());
    }

    void HandleInput(uint32_t key) override {}
};