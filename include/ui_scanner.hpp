#pragma once
#include "ui_pages.hpp"
#include "ui_theme.hpp"
#include "engine_state.hpp"
#include "mokuren.hpp"
#include <vector>
#include <string>
#include <algorithm>
#include <mutex>
#include <thread>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <sstream>
#include <map>
#include <set>

using json = nlohmann::json;

class UIScanner : public UIPage {
private:
    EngineState* state;
    Mokuren* engine;
    std::string config_path; 
    
    int selected_run = 0; 
    int scroll_offset = 0; 
    bool show_popup = false; 
    
    // --- NEU: Heatmap State ---
    bool show_heatmap = false;
    int hm_cursor_x = 0;
    int hm_cursor_y = 0;
    // --------------------------

    std::function<void(const UIResult&)> on_inspect; 

public:
    UIScanner(EngineState* s, Mokuren* e, const std::string& cfg_path, std::function<void(const UIResult&)> inspect_cb) 
        : state(s), engine(e), config_path(cfg_path), on_inspect(inspect_cb) {}

    void Render(struct ncplane* stdplane) override {
        unsigned int dimy, dimx;
        ncplane_dim_yx(stdplane, &dimy, &dimx);

        std::string current_status = state->GetStatus();
        int current_perm = state->current_permutation.load();
        int total_perm = state->total_permutations.load();
        bool is_running = state->is_running.load();

        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, 10, 3, "[ SYSTEM DIAGNOSTICS ]");
        ncplane_putstr_yx(stdplane, 10, 40, "[ HYPERPARAMETER MATRIX ]");
        
        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 11, 3, "LINK   :");
        UITheme::StyleDataValue(stdplane);
        ncplane_putstr_yx(stdplane, 11, 12, "ESTABLISHED");
        
        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 12, 3, "WORKER :");
        if (is_running) {
            UITheme::StyleCursorActive(stdplane);
            ncplane_putstr_yx(stdplane, 12, 12, "ACTIVE");
        } else {
            UITheme::StyleTextMuted(stdplane);
            ncplane_putstr_yx(stdplane, 12, 12, "SLEEPING");
        }

        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 13, 3, "MODE   :");
        UITheme::StyleDataValue(stdplane);
        ncplane_putstr_yx(stdplane, 13, 12, is_running ? "[ SCANNING ]" : "PRESS [S] OR [W]");

        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 11, 40, "STATUS:");
        UITheme::StyleAlert(stdplane);
        ncplane_putstr_yx(stdplane, 11, 48, current_status.c_str());

        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 12, 40, "PERMS :");
        UITheme::StyleTextDefault(stdplane);
        std::string perm_text = std::to_string(current_perm) + " / " + std::to_string(total_perm);
        ncplane_putstr_yx(stdplane, 12, 48, perm_text.c_str());

        int max_bar = std::max(10, (int)dimx - 48 - 5); 
        int bar_width = std::min(40, max_bar);
        int filled = total_perm > 0 ? (current_perm * bar_width) / total_perm : 0;
        std::string bar = "[";
        for(int i = 0; i < bar_width; i++) bar += (i < filled) ? "#" : ".";
        bar += "]";
        
        (is_running) ? UITheme::StyleCursorActive(stdplane) : UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 13, 40, bar.c_str());

        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, 15, 3, "[ LEADERBOARD : TOP 100 ]");
        
        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 16, 3, " RANK  PROFIT      TP%    BE%    SL%   DD%    SHARPE SORTINO MC_DD  TRADES");
        std::string l_hline = std::string(dimx > 6 ? dimx - 6 : 10, '-');
        ncplane_putstr_yx(stdplane, 17, 3, l_hline.c_str());

        std::vector<UIResult> top_runs;
        {
            std::lock_guard<std::mutex> lock(state->ui_mutex);
            top_runs = state->top_results; 
        }

        int limit = top_runs.size();
        if (selected_run >= limit && limit > 0) selected_run = limit - 1;
        if (selected_run < 0) selected_run = 0;

        int max_visible = std::max(1, (int)dimy - 19); 
        int scroll_margin = 1; 

        if (selected_run < scroll_offset + scroll_margin) {
            scroll_offset = std::max(0, selected_run - scroll_margin);
        } else if (selected_run >= scroll_offset + max_visible - scroll_margin) {
            scroll_offset = std::min(std::max(0, limit - max_visible), selected_run - max_visible + scroll_margin + 1);
        }

        for(int i = 0; i < max_visible && (scroll_offset + i) < limit; i++) {
            int idx = scroll_offset + i;
            bool is_active = (idx == selected_run);
            
            if (is_active) {
                UITheme::StyleCursorActive(stdplane);
                ncplane_putstr_yx(stdplane, 18 + i, 1, ">");
                UITheme::StyleDataValue(stdplane); 
            } else {
                UITheme::StyleTextMuted(stdplane);
                ncplane_putstr_yx(stdplane, 18 + i, 1, " ");
                UITheme::StyleTextDefault(stdplane);
            }

            char buf[256];
            snprintf(buf, sizeof(buf), "%-4d  $%-10.2f %-4.1f%% %-4.1f%% %-4.1f%% -%-6.2f%% %-6.2f %-6.2f -%-5.2f%% %-5d", 
                     idx + 1, top_runs[idx].net_profit, 
                     top_runs[idx].tp_pct, top_runs[idx].be_pct, top_runs[idx].sl_pct, 
                     top_runs[idx].max_drawdown, 
                     top_runs[idx].sharpe, top_runs[idx].sortino, top_runs[idx].mc_drawdown, top_runs[idx].trades);
            ncplane_putstr_yx(stdplane, 18 + i, 4, buf);
        }

        if (!is_running) {
            UITheme::StyleAlert(stdplane); 
            // Hint Update für Heatmap
            ncplane_putstr_yx(stdplane, dimy - 1, 3, "[S] SCAN  [W] WFA  [H] HEATMAP  [D] DETAILS  [UP/DN] SELECT  [ENTER] REPLAY");
        }

        // ==========================================
        // TEXT-OVERLAY (DETAILS)
        // ==========================================
        if (show_popup && !top_runs.empty() && selected_run < top_runs.size()) {
            int p_w = 60; int p_h = 16;
            int p_y = (dimy - p_h) / 2; int p_x = (dimx - p_w) / 2;

            UITheme::StyleBackground(stdplane);
            for(int i = 0; i < p_h; i++) ncplane_putstr_yx(stdplane, p_y + i, p_x, std::string(p_w, ' ').c_str());
            
            UITheme::StyleDataValue(stdplane);
            ncplane_putstr_yx(stdplane, p_y, p_x, std::string(p_w, '=').c_str());
            ncplane_putstr_yx(stdplane, p_y + p_h - 1, p_x, std::string(p_w, '=').c_str());
            for(int i = 1; i < p_h - 1; i++) {
                ncplane_putstr_yx(stdplane, p_y + i, p_x, "|");
                ncplane_putstr_yx(stdplane, p_y + i, p_x + p_w - 1, "|");
            }

            UITheme::StyleTextDefault(stdplane);
            ncplane_putstr_yx(stdplane, p_y + 1, p_x + 2, "[ HYPERPARAMETER DETAILS ]");
            
            UITheme::StyleTextMuted(stdplane);
            std::string raw_params = top_runs[selected_run].params_str;
            int line_y = p_y + 3;
            
            size_t start = 0; size_t end = raw_params.find(' ');
            while (end != std::string::npos) {
                std::string p = raw_params.substr(start, end - start);
                if (!p.empty()) {
                    ncplane_putstr_yx(stdplane, line_y++, p_x + 4, p.c_str());
                    if (line_y >= p_y + p_h - 3) break; 
                }
                start = end + 1; end = raw_params.find(' ', start);
            }
            if (start < raw_params.length() && line_y < p_y + p_h - 3) {
                ncplane_putstr_yx(stdplane, line_y, p_x + 4, raw_params.substr(start).c_str());
            }

            UITheme::StyleAlert(stdplane);
            ncplane_putstr_yx(stdplane, p_y + p_h - 2, p_x + 2, "[ANY KEY] CLOSE OVERLAY");
        }

        // ==========================================
        // 2D RGB HEATMAP OVERLAY
        // ==========================================
        if (show_heatmap && !top_runs.empty()) {
            std::map<std::string, std::set<double>> param_space;
            std::vector<std::map<std::string, double>> run_params(limit);

            double min_profit = top_runs[0].net_profit;
            double max_profit = top_runs[0].net_profit;

            // 1. Min/Max extrahieren & Parameter auslesen
            for (int i = 0; i < limit; i++) {
                if (top_runs[i].net_profit < min_profit) min_profit = top_runs[i].net_profit;
                if (top_runs[i].net_profit > max_profit) max_profit = top_runs[i].net_profit;

                std::stringstream ss(top_runs[i].params_str);
                std::string token;
                while (ss >> token) {
                    size_t eq = token.find('=');
                    if (eq != std::string::npos) {
                        std::string k = token.substr(0, eq);
                        try {
                            double v = std::stod(token.substr(eq+1));
                            param_space[k].insert(v);
                            run_params[i][k] = v;
                        } catch(...) {}
                    }
                }
            }

            // 2. Finde die zwei am stärksten variierenden Achsen
            std::vector<std::pair<std::string, size_t>> sorted_params;
            for (auto& [k, v_set] : param_space) sorted_params.push_back({k, v_set.size()});
            std::sort(sorted_params.begin(), sorted_params.end(), [](auto& a, auto& b){ return a.second > b.second; });

            std::string x_param = sorted_params.size() > 0 ? sorted_params[0].first : "NONE";
            std::string y_param = sorted_params.size() > 1 ? sorted_params[1].first : "NONE";

            std::vector<double> x_vals, y_vals;
            if (sorted_params.size() > 0) x_vals.assign(param_space[x_param].begin(), param_space[x_param].end());
            if (sorted_params.size() > 1) y_vals.assign(param_space[y_param].begin(), param_space[y_param].end());
            if (x_vals.empty()) x_vals.push_back(0.0);
            if (y_vals.empty()) y_vals.push_back(0.0);

            // Cursor clamping
            if (hm_cursor_x < 0) hm_cursor_x = 0;
            if (hm_cursor_x >= x_vals.size()) hm_cursor_x = x_vals.size() - 1;
            if (hm_cursor_y < 0) hm_cursor_y = 0;
            if (hm_cursor_y >= y_vals.size()) hm_cursor_y = y_vals.size() - 1;

            // 3. UI Box berechnen
            int p_w = std::min(90, (int)dimx - 6);
            int p_h = std::min(30, (int)dimy - 4);
            int p_y = (dimy - p_h) / 2;
            int p_x = (dimx - p_w) / 2;

            UITheme::StyleBackground(stdplane);
            for(int i = 0; i < p_h; i++) ncplane_putstr_yx(stdplane, p_y + i, p_x, std::string(p_w, ' ').c_str());
            
            UITheme::StyleDataValue(stdplane);
            ncplane_putstr_yx(stdplane, p_y, p_x, std::string(p_w, '=').c_str());
            ncplane_putstr_yx(stdplane, p_y + p_h - 1, p_x, std::string(p_w, '=').c_str());
            for(int i = 1; i < p_h - 1; i++) {
                ncplane_putstr_yx(stdplane, p_y + i, p_x, "|");
                ncplane_putstr_yx(stdplane, p_y + i, p_x + p_w - 1, "|");
            }

            UITheme::StyleTextDefault(stdplane);
            ncplane_putstr_yx(stdplane, p_y + 1, p_x + 2, "[ 2D PROFIT HEATMAP ]");
            
            UITheme::StyleTextMuted(stdplane);
            ncplane_putstr_yx(stdplane, p_y + 2, p_x + 2, ("Y: " + y_param + " | X: " + x_param).c_str());

            // 4. Scroll-Offsets für große Grids berechnen
            int max_x_cells = (p_w - 16) / 4;
            int max_y_cells = p_h - 9;
            
            int hm_scroll_x = std::max(0, hm_cursor_x - max_x_cells/2);
            if (hm_scroll_x > (int)x_vals.size() - max_x_cells) hm_scroll_x = std::max(0, (int)x_vals.size() - max_x_cells);
            
            int hm_scroll_y = std::max(0, hm_cursor_y - max_y_cells/2);
            if (hm_scroll_y > (int)y_vals.size() - max_y_cells) hm_scroll_y = std::max(0, (int)y_vals.size() - max_y_cells);

            // 5. Grid Rendern
            for (int cy = 0; cy < max_y_cells && (hm_scroll_y + cy) < y_vals.size(); cy++) {
                int actual_y = hm_scroll_y + cy;
                
                // Y-Label
                char y_lbl[16]; snprintf(y_lbl, sizeof(y_lbl), "%-6.2f", y_vals[actual_y]);
                UITheme::StyleTextMuted(stdplane);
                ncplane_putstr_yx(stdplane, p_y + 4 + cy, p_x + 2, y_lbl);

                for (int cx = 0; cx < max_x_cells && (hm_scroll_x + cx) < x_vals.size(); cx++) {
                    int actual_x = hm_scroll_x + cx;
                    int draw_x = p_x + 10 + (cx * 4);
                    int draw_y = p_y + 4 + cy;

                    // Match im Data-Set suchen
                    int match_idx = -1;
                    for (int i=0; i<limit; i++) {
                        bool m = true;
                        if(sorted_params.size() > 0 && run_params[i][x_param] != x_vals[actual_x]) m = false;
                        if(sorted_params.size() > 1 && run_params[i][y_param] != y_vals[actual_y]) m = false;
                        if(m) { match_idx = i; break; }
                    }

                    if (match_idx != -1) {
                        double profit = top_runs[match_idx].net_profit;
                        double norm = (max_profit > min_profit) ? (profit - min_profit) / (max_profit - min_profit) : 0.5;

                        if (actual_x == hm_cursor_x && actual_y == hm_cursor_y) {
                            UITheme::StyleHeatmapCell(stdplane, norm);
                            ncplane_putstr_yx(stdplane, draw_y, draw_x, "██");
                            // Cursor Klammern (Weiß)
                            ncplane_set_fg_rgb8(stdplane, 255, 255, 255);
                            ncplane_set_bg_default(stdplane);
                            ncplane_putstr_yx(stdplane, draw_y, draw_x - 1, "[");
                            ncplane_putstr_yx(stdplane, draw_y, draw_x + 2, "]");
                        } else {
                            UITheme::StyleHeatmapCell(stdplane, norm);
                            ncplane_putstr_yx(stdplane, draw_y, draw_x, "██");
                        }
                    } else {
                        // Fehlende Parameter-Kombi im Grid (z.B. weil Grid abgewürgt wurde)
                        UITheme::StyleTextMuted(stdplane);
                        ncplane_putstr_yx(stdplane, draw_y, draw_x, "░░");
                    }
                }
            }

            // 6. X-Labels (Nur den aktiven Cursor unten anzeigen um Platz zu sparen)
            char x_lbl[32]; snprintf(x_lbl, sizeof(x_lbl), "^ %s = %.2f", x_param.c_str(), x_vals[hm_cursor_x]);
            UITheme::StyleTextMuted(stdplane);
            ncplane_putstr_yx(stdplane, p_y + 4 + max_y_cells, p_x + 10 + ((hm_cursor_x - hm_scroll_x) * 4), "^");

            // 7. Ausgewählte Metriken ganz unten anzeigen
            int match_idx = -1;
            for (int i=0; i<limit; i++) {
                bool m = true;
                if(sorted_params.size() > 0 && run_params[i][x_param] != x_vals[hm_cursor_x]) m = false;
                if(sorted_params.size() > 1 && run_params[i][y_param] != y_vals[hm_cursor_y]) m = false;
                if(m) { match_idx = i; break; }
            }

            if (match_idx != -1) {
                char info[128];
                snprintf(info, sizeof(info), "PNL: $%-8.2f | WINRATE: %-4.1f%% | DD: %-5.2f%% | TRADES: %d",
                    top_runs[match_idx].net_profit, top_runs[match_idx].winrate, 
                    top_runs[match_idx].max_drawdown, top_runs[match_idx].trades);
                
                UITheme::StyleCursorActive(stdplane);
                ncplane_putstr_yx(stdplane, p_y + p_h - 4, p_x + 2, info);
                
                UITheme::StyleTextMuted(stdplane);
                std::string p_str = top_runs[match_idx].params_str;
                if(p_str.length() > p_w - 6) p_str = p_str.substr(0, p_w - 9) + "...";
                ncplane_putstr_yx(stdplane, p_y + p_h - 3, p_x + 2, p_str.c_str());
            }

            UITheme::StyleAlert(stdplane);
            ncplane_putstr_yx(stdplane, p_y + p_h - 2, p_x + 2, "[ARROWS] NAVIGATE   [ENTER] REPLAY CELL   [ESC/H] CLOSE");
        }
    }

    void HandleInput(uint32_t key) override {
        // --- NEU: Heatmap Toggle ---
        if ((key == 'h' || key == 'H') && !state->is_running.load()) {
            show_heatmap = !show_heatmap;
            hm_cursor_x = 0; hm_cursor_y = 0;
            return;
        }

        // --- NEU: Heatmap Navigation Intercept ---
        if (show_heatmap) {
            if (key == NCKEY_ESC || key == 'q' || key == 'Q') {
                show_heatmap = false;
                return;
            }
            if (key == NCKEY_UP) hm_cursor_y--;
            if (key == NCKEY_DOWN) hm_cursor_y++;
            if (key == NCKEY_LEFT) hm_cursor_x--;
            if (key == NCKEY_RIGHT) hm_cursor_x++;
            
            if (key == NCKEY_ENTER && !state->is_running.load()) {
                // Finde genau den Datensatz für die selektierte Zelle und starte Replay
                std::vector<UIResult> top_runs;
                {
                    std::lock_guard<std::mutex> lock(state->ui_mutex);
                    top_runs = state->top_results; 
                }
                
                // Wir müssen die Achsen kurz replizieren um den match zu finden
                std::map<std::string, std::set<double>> param_space;
                std::vector<std::map<std::string, double>> run_params(top_runs.size());
                for (size_t i = 0; i < top_runs.size(); i++) {
                    std::stringstream ss(top_runs[i].params_str);
                    std::string token;
                    while (ss >> token) {
                        size_t eq = token.find('=');
                        if (eq != std::string::npos) {
                            try {
                                run_params[i][token.substr(0, eq)] = std::stod(token.substr(eq+1));
                                param_space[token.substr(0, eq)].insert(std::stod(token.substr(eq+1)));
                            } catch(...) {}
                        }
                    }
                }
                std::vector<std::pair<std::string, size_t>> sorted_params;
                for (auto& [k, v_set] : param_space) sorted_params.push_back({k, v_set.size()});
                std::sort(sorted_params.begin(), sorted_params.end(), [](auto& a, auto& b){ return a.second > b.second; });

                std::string x_param = sorted_params.size() > 0 ? sorted_params[0].first : "NONE";
                std::string y_param = sorted_params.size() > 1 ? sorted_params[1].first : "NONE";
                std::vector<double> x_vals, y_vals;
                if (sorted_params.size() > 0) x_vals.assign(param_space[x_param].begin(), param_space[x_param].end());
                if (sorted_params.size() > 1) y_vals.assign(param_space[y_param].begin(), param_space[y_param].end());
                
                int c_x = std::clamp(hm_cursor_x, 0, (int)x_vals.size()-1);
                int c_y = std::clamp(hm_cursor_y, 0, (int)y_vals.size()-1);

                for (size_t i=0; i<top_runs.size(); i++) {
                    bool m = true;
                    if(sorted_params.size() > 0 && run_params[i][x_param] != x_vals[c_x]) m = false;
                    if(sorted_params.size() > 1 && run_params[i][y_param] != y_vals[c_y]) m = false;
                    if(m && !top_runs[i].full_config.empty()) { 
                        on_inspect(top_runs[i]); 
                        show_heatmap = false;
                        return; 
                    }
                }
            }
            return; // Blockiert restlichen Input, wenn Heatmap offen ist
        }
        // ------------------------------------------

        if (key == 'd' || key == 'D') {
            show_popup = !show_popup;
            return;
        }

        if (show_popup) {
            if (key == NCKEY_ESC || key == NCKEY_ENTER || key == 'q' || key == 'Q') {
                show_popup = false;
                return;
            }
            if (key != NCKEY_UP && key != NCKEY_DOWN) {
                return; 
            }
        }

        if ((key == 's' || key == 'S' || key == 'w' || key == 'W') && !state->is_running.load()) {
            bool run_wfa = (key == 'w' || key == 'W'); 
            
            state->is_running = true;
            state->current_permutation = 0;
            state->total_permutations = 0;
            selected_run = 0;
            
            {
                std::lock_guard<std::mutex> lock(state->ui_mutex);
                state->top_results.clear();
            }

            EngineState* bg_state = state;
            Mokuren* bg_engine = engine;
            std::string bg_config = config_path;

            std::thread([bg_state, bg_engine, bg_config, run_wfa]() {
                std::string data_path = "binance/monthly/DEFAULT.csv"; 
                try {
                    std::ifstream file(bg_config);
                    if (file.is_open()) {
                        json j = json::parse(file);
                        std::function<void(const json&)> find_path = [&](const json& node) {
                            if (node.is_object()) {
                                for (auto& [k, v] : node.items()) {
                                    if (v.is_string() && (k == "filepath" || k == "data_path")) {
                                        data_path = v.get<std::string>(); 
                                    } else { find_path(v); }
                                }
                            }
                        };
                        find_path(j);
                    }
                } catch (...) {}

                auto status_cb = [bg_state](const std::string& status) { bg_state->SetStatus(status); };
                auto progress_cb = [bg_state](int current, int total) { bg_state->current_permutation = current; bg_state->total_permutations = total; };
                
                auto result_cb = [bg_state](const TestResult& res) {
                    UIResult ur;
                    ur.net_profit = res.net_profit;
                    ur.winrate = res.winrate;
                    ur.tp_pct = res.tp_pct;
                    ur.be_pct = res.be_pct;
                    ur.sl_pct = res.sl_pct;
                    ur.max_drawdown = res.max_drawdown; 
                    ur.trades = res.trades;
                    
                    ur.sharpe = res.sharpe;
                    ur.sortino = res.sortino;
                    ur.mc_drawdown = res.mc_drawdown;

                    ur.full_config = res.full_config; 
                    ur.trade_log = res.trade_log; 
                    ur.wfa_configs = res.wfa_configs;
                    
                    std::string p_str;
                    for (const auto& [k, v] : res.parameters) {
                        char buf[32]; snprintf(buf, sizeof(buf), "%g", v);
                        p_str += k + "=" + std::string(buf) + " ";
                    }
                    ur.params_str = p_str;
                    
                    std::lock_guard<std::mutex> lock(bg_state->ui_mutex);
                    bg_state->top_results.push_back(ur);
                    
                    std::sort(bg_state->top_results.begin(), bg_state->top_results.end(), [](const UIResult& a, const UIResult& b) {
                        return a.net_profit > b.net_profit;
                    });
                    if (bg_state->top_results.size() > 100) {
                        bg_state->top_results.pop_back();
                    }
                };

                if (run_wfa) bg_engine->RunWFA(data_path, bg_config, status_cb, progress_cb, result_cb);
                else bg_engine->RunGridSearch(data_path, bg_config, status_cb, progress_cb, result_cb);

                bg_state->is_running = false;
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (!bg_state->is_running.load()) bg_state->SetStatus("IDLE");
                
            }).detach();
        }

        int limit = 0;
        {
            std::lock_guard<std::mutex> lock(state->ui_mutex);
            limit = state->top_results.size();
        }

        if (key == NCKEY_UP && selected_run > 0) selected_run--;
        if (key == NCKEY_DOWN && selected_run < limit - 1) selected_run++;
        
        if (key == NCKEY_ENTER && !state->is_running.load()) {
            UIResult selected_res;
            {
                std::lock_guard<std::mutex> lock(state->ui_mutex);
                if (selected_run < state->top_results.size()) { 
                    selected_res = state->top_results[selected_run]; 
                }
            }
            if (!selected_res.full_config.empty()) { 
                on_inspect(selected_res); 
                return; 
            }
        }
    }
};