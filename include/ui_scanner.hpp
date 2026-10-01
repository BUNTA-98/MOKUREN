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

using json = nlohmann::json;

class UIScanner : public UIPage {
private:
    EngineState* state;
    Mokuren* engine;
    std::string config_path = "config.json"; 
    
    int selected_run = 0; 
    int scroll_offset = 0; 
    bool show_popup = false; 
    std::function<void(const UIResult&)> on_inspect; 

public:
    UIScanner(EngineState* s, Mokuren* e, std::function<void(const UIResult&)> inspect_cb) 
        : state(s), engine(e), on_inspect(inspect_cb) {}

    // core rendering loop for scanner ui
    void Render(struct ncplane* stdplane) override {
        unsigned int dimy, dimx;
        ncplane_dim_yx(stdplane, &dimy, &dimx);

        std::string current_status = state->GetStatus();
        int current_perm = state->current_permutation.load();
        int total_perm = state->total_permutations.load();
        bool is_running = state->is_running.load();

        // draw top diagnostic headers
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

        // draw progress bar
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

        // lock mutex to safely read results from background thread
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

        // scroll clamping mechanics
        if (selected_run < scroll_offset + scroll_margin) {
            scroll_offset = std::max(0, selected_run - scroll_margin);
        } else if (selected_run >= scroll_offset + max_visible - scroll_margin) {
            scroll_offset = std::min(std::max(0, limit - max_visible), selected_run - max_visible + scroll_margin + 1);
        }

        // render leaderboard rows
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
            ncplane_putstr_yx(stdplane, dimy - 1, 3, "[S] GRID SCAN   [W] WFA SCAN   [UP/DOWN] SELECT   [D] DETAILS   [ENTER] REPLAY");
        }

        // render parameter details popup overlay
        if (show_popup && !top_runs.empty() && selected_run < top_runs.size()) {
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
            ncplane_putstr_yx(stdplane, p_y + 1, p_x + 2, "[ HYPERPARAMETER DETAILS ]");
            
            UITheme::StyleTextMuted(stdplane);
            std::string raw_params = top_runs[selected_run].params_str;
            int line_y = p_y + 3;
            
            size_t start = 0;
            size_t end = raw_params.find(' ');
            while (end != std::string::npos) {
                std::string p = raw_params.substr(start, end - start);
                if (!p.empty()) {
                    ncplane_putstr_yx(stdplane, line_y++, p_x + 4, p.c_str());
                    if (line_y >= p_y + p_h - 3) break; 
                }
                start = end + 1;
                end = raw_params.find(' ', start);
            }
            
            if (start < raw_params.length() && line_y < p_y + p_h - 3) {
                ncplane_putstr_yx(stdplane, line_y, p_x + 4, raw_params.substr(start).c_str());
            }

            UITheme::StyleAlert(stdplane);
            ncplane_putstr_yx(stdplane, p_y + p_h - 2, p_x + 2, "[ANY KEY] CLOSE OVERLAY");
        }
    }

    void HandleInput(uint32_t key) override {
        // toggle details popup overlay
        if (key == 'd' || key == 'D') {
            show_popup = !show_popup;
            return;
        }

        // capture input context if popup is active
        if (show_popup) {
            if (key == NCKEY_ESC || key == NCKEY_ENTER || key == 'q' || key == 'Q') {
                show_popup = false;
                return;
            }
            if (key != NCKEY_UP && key != NCKEY_DOWN) {
                return; 
            }
        }

        // initiate grid or wfa scan
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

            // spawn dedicated background thread to unblock rendering loop
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

                if (run_wfa) {
                    bg_engine->RunWFA(data_path, bg_config, status_cb, progress_cb, result_cb);
                } else {
                    bg_engine->RunGridSearch(data_path, bg_config, status_cb, progress_cb, result_cb);
                }

                // unlock ui explicitly
                bg_state->is_running = false;

                // delay idle status reset so user can read fast error messages 
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (!bg_state->is_running.load()) {
                    bg_state->SetStatus("IDLE");
                }
            }).detach();
        }

        int limit = 0;
        {
            std::lock_guard<std::mutex> lock(state->ui_mutex);
            limit = state->top_results.size();
        }

        if (key == NCKEY_UP && selected_run > 0) selected_run--;
        if (key == NCKEY_DOWN && selected_run < limit - 1) selected_run++;
        
        // inspect selected permutation via ui redirect
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