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

public:
    // init scanner with main engine refs
    UIScanner(EngineState* s, Mokuren* e) : state(s), engine(e) {}

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
        ncplane_putstr_yx(stdplane, 19, 3, "RANK  PROFIT       WINRATE   TRADES   PARAMETERS");
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
        for(int i = 0; i < limit; i++) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%-4d  $%-10.2f %-7.1f%% %-8d %s", 
                     i + 1, top_runs[i].net_profit, top_runs[i].winrate, 
                     top_runs[i].trades, top_runs[i].params_str.c_str());
                     
            if (i == 0) UITheme::StyleDataValue(stdplane); else UITheme::StyleTextDefault(stdplane);
            ncplane_putstr_yx(stdplane, 21 + i, 3, buf);
        }

        if (!is_running) UITheme::StyleAlert(stdplane); else UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 27, 3, "[S] INITIATE OVERRIDE");
    }

    void HandleInput(uint32_t key) override {
        // start grid search backtest on [s]
        if ((key == 's' || key == 'S') && !state->is_running.load()) {
            state->is_running = true;
            state->current_permutation = 0;
            state->total_permutations = 0;
            {
                std::lock_guard<std::mutex> lock(state->ui_mutex);
                state->top_results.clear();
            }

            std::thread([this]() {
                std::string data_path = "binance/monthly/DEFAULT.csv"; // fallback path
                
                // deep search for filepath or data_path inside json
                try {
                    std::ifstream file(config_path);
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

                // feed engine with resolved path
                engine->RunGridSearch(data_path, config_path, 
                    [this](const std::string& status) { state->SetStatus(status); },
                    [this](int current, int total) { state->current_permutation = current; state->total_permutations = total; },
                    [this](const TestResult& res) {
                        UIResult ur;
                        ur.net_profit = res.net_profit;
                        ur.winrate = res.winrate;
                        ur.trades = res.trades;
                        
                        std::string p_str;
                        for (const auto& [k, v] : res.parameters) {
                            char buf[32];
                            snprintf(buf, sizeof(buf), "%g", v);
                            p_str += k + "=" + std::string(buf) + " ";
                        }
                        ur.params_str = p_str;
                        
                        std::lock_guard<std::mutex> lock(state->ui_mutex);
                        state->top_results.push_back(ur);
                    }
                );
                
                state->is_running = false;
                state->SetStatus("IDLE");
            }).detach();
        }
    }
};
