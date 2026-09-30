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
    std::function<void(const UIResult&)> on_inspect; 

public:
    UIScanner(EngineState* s, Mokuren* e, std::function<void(const UIResult&)> inspect_cb) 
        : state(s), engine(e), on_inspect(inspect_cb) {}

    void Render(struct ncplane* stdplane) override {
        unsigned int dimy, dimx;
        ncplane_dim_yx(stdplane, &dimy, &dimx);

        std::string current_status = state->GetStatus();
        int current_perm = state->current_permutation.load();
        int total_perm = state->total_permutations.load();
        bool is_running = state->is_running.load();

        // --- TOP SECTION: Diagnostics & Matrix Info ---
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

        // --- PROGRESS BAR ---
        int max_bar = std::max(10, (int)dimx - 48 - 5); 
        int bar_width = std::min(40, max_bar);
        int filled = total_perm > 0 ? (current_perm * bar_width) / total_perm : 0;
        std::string bar = "[";
        for(int i = 0; i < bar_width; i++) bar += (i < filled) ? "#" : ".";
        bar += "]";
        
        (is_running) ? UITheme::StyleCursorActive(stdplane) : UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 13, 40, bar.c_str());

        // --- LOWER SECTION: Live Leaderboard ---
        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, 15, 3, "[ LIVE LEADERBOARD : TOP 100 ]");
        
        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 16, 3, "   RANK  PROFIT       TP%     BE%     SL%     DRAWDOWN  TRADES   PARAMETERS");
        std::string l_hline = std::string(dimx > 6 ? dimx - 6 : 10, '-');
        ncplane_putstr_yx(stdplane, 17, 3, l_hline.c_str());

        std::vector<UIResult> top_runs;
        {
            // Thread-safe copy of top results for rendering
            std::lock_guard<std::mutex> lock(state->ui_mutex);
            top_runs = state->top_results; 
        }

        int limit = top_runs.size();
        if (selected_run >= limit && limit > 0) selected_run = limit - 1;
        if (selected_run < 0) selected_run = 0;

        // --- DYNAMIC SCROLL LOGIC ---
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
            snprintf(buf, sizeof(buf), "%-4d  $%-10.2f %-5.1f%% %-5.1f%% %-5.1f%% -%-8.2f%% %-8d %s", 
                     idx + 1, top_runs[idx].net_profit, 
                     top_runs[idx].tp_pct, top_runs[idx].be_pct, top_runs[idx].sl_pct, 
                     top_runs[idx].max_drawdown, top_runs[idx].trades, top_runs[idx].params_str.c_str());
            ncplane_putstr_yx(stdplane, 18 + i, 4, buf);
        }

        // --- FOOTER ---
        if (!is_running) {
            UITheme::StyleAlert(stdplane); 
            ncplane_putstr_yx(stdplane, dimy - 1, 3, "[S] GRID SCAN   [W] WFA SCAN   [UP/DOWN] SELECT   [ENTER] REPLAY");
        }
    }

    void HandleInput(uint32_t key) override {
        // --- LAUNCH SCAN THREAD (Grid or WFA) ---
        if ((key == 's' || key == 'S' || key == 'w' || key == 'W') && !state->is_running.load()) {
            bool run_wfa = (key == 'w' || key == 'W'); 
            
            state->is_running = true;
            state->current_permutation = 0;
            state->total_permutations = 0;
            {
                std::lock_guard<std::mutex> lock(state->ui_mutex);
                state->top_results.clear();
            }

            EngineState* bg_state = state;
            Mokuren* bg_engine = engine;
            std::string bg_config = config_path;

            std::thread([bg_state, bg_engine, bg_config, run_wfa]() {
                std::string data_path = "binance/monthly/DEFAULT.csv"; 
                
                // Parse config safely to locate the historical data path
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

                // Define callbacks to update the UI safely from background thread
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
                    ur.full_config = res.full_config; 
                    ur.trade_log = res.trade_log; 
                    
                    // Format parameters for the leaderboard view
                    std::string p_str;
                    for (const auto& [k, v] : res.parameters) {
                        char buf[32]; snprintf(buf, sizeof(buf), "%g", v);
                        p_str += k + "=" + std::string(buf) + " ";
                    }
                    ur.params_str = p_str;
                    
                    std::lock_guard<std::mutex> lock(bg_state->ui_mutex);
                    bg_state->top_results.push_back(ur);
                    
                    // Keep the top 100 runs to preserve memory and rendering performance
                    std::sort(bg_state->top_results.begin(), bg_state->top_results.end(), [](const UIResult& a, const UIResult& b) {
                        return a.net_profit > b.net_profit;
                    });
                    if (bg_state->top_results.size() > 100) {
                        bg_state->top_results.pop_back();
                    }
                };

                // Route execution based on selected mode
                if (run_wfa) {
                    bg_engine->RunWFA(data_path, bg_config, status_cb, progress_cb, result_cb);
                } else {
                    bg_engine->RunGridSearch(data_path, bg_config, status_cb, progress_cb, result_cb);
                }

                bg_state->is_running = false;
               // bg_state->SetStatus("IDLE");
            }).detach();
        }

        // --- NAVIGATION ---
        int limit = 0;
        {
            std::lock_guard<std::mutex> lock(state->ui_mutex);
            limit = state->top_results.size();
        }

        if (key == NCKEY_UP && selected_run > 0) selected_run--;
        if (key == NCKEY_DOWN && selected_run < limit - 1) selected_run++;
        
        // --- EXECUTE REPLAY ---
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
