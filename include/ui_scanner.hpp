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
    std::function<void(const json&)> on_inspect; 

public:
    UIScanner(EngineState* s, Mokuren* e, std::function<void(const json&)> inspect_cb) 
        : state(s), engine(e), on_inspect(inspect_cb) {}

    void Render(struct ncplane* stdplane) override {
        std::string current_status = state->GetStatus();
        int current_perm = state->current_permutation.load();
        int total_perm = state->total_permutations.load();
        bool is_running = state->is_running.load();

        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, 7, 3, "[ SYSTEM DIAGNOSTICS ]");
        
        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 9, 3, "CORE_LINK     :");
        UITheme::StyleDataValue(stdplane);
        ncplane_putstr_yx(stdplane, 9, 19, "ESTABLISHED");
        
        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 10, 3, "WORKER_THREAD :");
        if (is_running) {
            UITheme::StyleCursorActive(stdplane);
            ncplane_putstr_yx(stdplane, 10, 19, "ACTIVE");
        } else {
            UITheme::StyleTextMuted(stdplane);
            ncplane_putstr_yx(stdplane, 10, 19, "SLEEPING");
        }

        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, 7, 40, "[ HYPERPARAMETER MATRIX ]");
        
        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 9, 40, "STATUS:");
        UITheme::StyleAlert(stdplane);
        ncplane_putstr_yx(stdplane, 9, 48, current_status.c_str());

        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 11, 40, "PERMUTATIONS :");
        UITheme::StyleTextDefault(stdplane);
        std::string perm_text = std::to_string(current_perm) + " / " + std::to_string(total_perm);
        ncplane_putstr_yx(stdplane, 11, 55, perm_text.c_str());

        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 13, 40, "PROGRESS:");
        
        int bar_width = 40;
        int filled = total_perm > 0 ? (current_perm * bar_width) / total_perm : 0;
        std::string bar = "[";
        for(int i = 0; i < bar_width; i++) bar += (i < filled) ? "#" : ".";
        bar += "]";
        
        if (is_running) UITheme::StyleCursorActive(stdplane);
        else UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 14, 40, bar.c_str());

        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, 17, 3, "[ LIVE LEADERBOARD : TOP 5 ]");
        
        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 19, 3, "   RANK  PROFIT       WINRATE   TRADES   PARAMETERS");
        ncplane_putstr_yx(stdplane, 20, 3, "--------------------------------------------------------------------------------");

        std::vector<UIResult> top_runs;
        {
            std::lock_guard<std::mutex> lock(state->ui_mutex);
            top_runs = state->top_results;
        }
        
        std::sort(top_runs.begin(), top_runs.end(), [](const UIResult& a, const UIResult& b) {
            return a.net_profit > b.net_profit;
        });

        int limit = std::min(static_cast<int>(top_runs.size()), 5);
        if (selected_run >= limit) selected_run = std::max(0, limit - 1);

        for(int i = 0; i < limit; i++) {
            bool is_active = (i == selected_run);
            
            if (is_active) {
                UITheme::StyleCursorActive(stdplane);
                ncplane_putstr_yx(stdplane, 21 + i, 1, ">");
                UITheme::StyleDataValue(stdplane); 
            } else {
                UITheme::StyleTextMuted(stdplane);
                ncplane_putstr_yx(stdplane, 21 + i, 1, " ");
                UITheme::StyleTextDefault(stdplane);
            }

            char buf[256];
            snprintf(buf, sizeof(buf), "%-4d  $%-10.2f %-7.1f%% %-8d %s", 
                     i + 1, top_runs[i].net_profit, top_runs[i].winrate, 
                     top_runs[i].trades, top_runs[i].params_str.c_str());
                     
            ncplane_putstr_yx(stdplane, 21 + i, 4, buf);
        }

        if (!is_running) {
            UITheme::StyleAlert(stdplane); 
            ncplane_putstr_yx(stdplane, 28, 3, "[S] START SCAN   [UP/DOWN] SELECT RUN   [ENTER] REPLAY & INSPECT");
        }
    }

    void HandleInput(uint32_t key) override {
        if ((key == 's' || key == 'S') && !state->is_running.load()) {
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

            std::thread([bg_state, bg_engine, bg_config]() {
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
                                    } else {
                                        find_path(v); 
                                    }
                                }
                            }
                        };
                        find_path(j);
                    }
                } catch (...) {}

                bg_engine->RunGridSearch(data_path, bg_config, 
                    [bg_state](const std::string& status) { bg_state->SetStatus(status); },
                    [bg_state](int current, int total) { bg_state->current_permutation = current; bg_state->total_permutations = total; },
                    [bg_state](const TestResult& res) {
                        UIResult ur;
                        ur.net_profit = res.net_profit;
                        ur.winrate = res.winrate;
                        ur.trades = res.trades;
                        ur.full_config = res.full_config; // <--- HIER übergeben
                        
                        std::string p_str;
                        for (const auto& [k, v] : res.parameters) {
                            char buf[32];
                            snprintf(buf, sizeof(buf), "%g", v);
                            p_str += k + "=" + std::string(buf) + " ";
                        }
                        ur.params_str = p_str;
                        
                        std::lock_guard<std::mutex> lock(bg_state->ui_mutex);
                        bg_state->top_results.push_back(ur);
                    }
                );
                
                bg_state->is_running = false;
                bg_state->SetStatus("IDLE");
            }).detach();
        }

        if (key == NCKEY_UP && selected_run > 0) selected_run--;
        if (key == NCKEY_DOWN && selected_run < 4) selected_run++;

        if (key == NCKEY_ENTER && !state->is_running.load()) {
            json cached_config;
            {
                std::lock_guard<std::mutex> lock(state->ui_mutex);
                if (selected_run < state->top_results.size()) {
                    cached_config = state->top_results[selected_run].full_config; 
                }
            }
            if (!cached_config.empty()) {
                on_inspect(cached_config); 
                return; 
            }
        }
    }
};