#pragma once
#include "ui_pages.hpp"
#include "ui_theme.hpp"
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

enum class MenuState {
    CATEGORIES,     
    SUBCATEGORIES,  
    PARAMETERS,     
    INLINE_EDIT,    
    SUBMENU_NAV,    
    SUBMENU_EDIT    
};

struct MenuRow {
    std::string display_path;
    json::json_pointer ptr;
    bool is_bool = false;
    bool is_number = false;
    bool is_string = false;
    bool is_range = false; 

    bool is_file_selector = false;
    std::vector<std::string> file_options;
    int current_file_idx = 0;
};

struct ParamConfigState {
    bool is_range = false;
    std::string val_str = "";
    std::string min_str = "";
    std::string max_str = "";
    std::string step_str = "";
    int cursor_y = 0; 
};

class PageConfig : public UIPage {
private:
    json config_data;
    
    std::vector<std::string> categories;
    std::map<std::string, std::vector<std::string>> subcategories;
    std::map<std::string, std::map<std::string, std::vector<MenuRow>>> grouped_rows;
    
    MenuState menu_state = MenuState::CATEGORIES;
    int category_cursor = 0;
    int subcat_cursor = 0;
    int param_cursor = 0;
    
    std::string edit_buffer = "";
    
    std::string config_path = "configs/default.json";
    
    ParamConfigState sub_state;

   json CreateDefaultConfig() {
        return json::parse(R"({
          "backtest": {
            "config_profile": "configs/default.json",
            "wfa_mode": false,
            "wfa_in_sample_days": 14,
            "wfa_out_of_sample_days": 7,
            "wfa_step_days": 7
          },
          "environment": {
            "dataset": "data",
            "interval_ms": 60000,
            "macro_interval_ms": 900000,
            "vwap_reset_hour": 0,
            "max_cores": 0,
            "tick_size": 0.1
          },
          "execution": {
            "taker_fee_pct": 0.0004,
            "slippage_pct": 0.0002
          },
          "filters": [
            { "active": true, "lookback": 1440, "name": "MacroTrend" },
            { "active": true, "min_volume": 2.0, "name": "MinVolume" },
            { "active": true, "lookback": 20, "name": "RVOL", "threshold": 2.0 },
            { "active": true, "lookback": 25.0, "min_atr": 40.0, "name": "ATRChop" },
            { "active": false, "lookback": 5, "min_dollar": 150.0, "name": "Volatility" },
            { "active": false, "end_h": 17, "end_m": 0, "name": "TimeOfDay", "start_h": 8, "start_m": 0 },
            { "active": false, "name": "POCTrend" },
            { "active": true, "name": "VwapTrend", "require_trend_alignment": true },
            { "active": true, "lookback": 5, "name": "CVDDivergence" }
          ],
          "risk": {
            "max_daily_loss": 400.0,
            "min_distance_dollars": 10.0,
            "modules": [
              { "active": true, "name": "SinglePositionLock" },
              { "active": true, "max_leverage": 10.0, "name": "MaxLeverageLock" },
              { "active": true, "cooldown_ms": 1800000, "name": "AntiRevengeLock" }
            ],
            "risk_per_trade_pct": 0.02,
            "sl_price_pct": 0.004,
            "tp_r": 3.0
          },
          "trade_management": {
            "break_even": { "enabled": false, "target_r": 0.25, "trigger_r": 1.5 },
            "scale_out": { "enabled": false, "fraction": 0.5, "trigger_r": 2.0 },
            "trailing": { "distance_r": 0.5, "enabled": false, "trigger_r": 2.5 }
          },
          "triggers": [
            { "active": true, "delta": 1.6, "name": "DeltaAbsorption" },
            { "active": false, "levels": 6.0, "name": "StackedImbalance", "ratio": 3.0 },
            { "active": false, "name": "SpoofHunter", "lookback_ms": 500, "min_wall_qty": 30.0, "drop_threshold": 0.9 }
          ]
        })");
    }

    std::string DoubleToStr(double d) {
        char buf[64]; snprintf(buf, sizeof(buf), "%g", d);
        return std::string(buf);
    }
    
    double ParseDouble(const std::string& str) {
        try { return std::stod(str); } catch (...) { return 0.0; }
    }

    std::vector<std::string> ScanDirectory(const std::string& folder, bool only_dirs = false) {
        std::vector<std::string> options;
        options.push_back(folder);
        try {
            if (fs::exists(folder)) {
                for (const auto& entry : fs::recursive_directory_iterator(folder)) {
                    if (only_dirs) {
                        if (entry.is_directory()) {
                            options.push_back(entry.path().string());
                        }
                    } else {
                        if (entry.is_regular_file() && 
                           (entry.path().extension() == ".csv" || entry.path().extension() == ".json")) {
                            options.push_back(entry.path().string());
                        }
                    }
                }
            }
        } catch (...) {}
        
        if (options.empty()) options.push_back(only_dirs ? "NO_DATASETS_FOUND" : "NO_FILES_FOUND");
        return options;
    }

    void FlattenJson(const json& j, const std::string& current_path, int level, 
                     const std::string& current_category, const std::string& current_subcategory) {
        if (j.is_object()) {
            if ((j.contains("mode") && j["mode"] == "range") || 
                (j.contains("min") && j.contains("max") && j.contains("step"))) {
                MenuRow row;
                std::string key = current_path.substr(current_path.find_last_of('/') + 1);
                row.display_path = key;
                row.ptr = json::json_pointer(current_path);
                row.is_number = true;
                row.is_range = true;
                grouped_rows[current_category][current_subcategory].push_back(row);
            } else {
                for (auto& [key, val] : j.items()) {
                    std::string next_cat = current_category;
                    std::string next_sub = current_subcategory;
                    
                    if (level == 0) {
                        next_cat = key;
                        next_sub = "General"; 
                    } else if (level == 1 && val.is_object() && !val.contains("mode") && !val.contains("min")) {
                        next_sub = key; 
                    }
                    
                    FlattenJson(val, current_path + "/" + key, level + 1, next_cat, next_sub);
                }
            }
        } else if (j.is_array()) {
            for (size_t i = 0; i < j.size(); ++i) {
                if (j[i].is_object() && j[i].contains("name")) {
                    std::string sub = j[i]["name"].get<std::string>();
                    FlattenJson(j[i], current_path + "/" + std::to_string(i), level + 1, current_category, sub);
                } else {
                    FlattenJson(j[i], current_path + "/" + std::to_string(i), level + 1, current_category, current_subcategory);
                }
            }
        } else {
            std::string key = current_path.substr(current_path.find_last_of('/') + 1);
            if (key == "name" && current_subcategory != "General") return; 

            MenuRow row;
            row.display_path = key;
            row.ptr = json::json_pointer(current_path);
            row.is_bool = j.is_boolean();
            row.is_number = j.is_number();
            
            if (j.is_string()) {
                row.is_string = true;
                
                // --- NEU: Dataset Ordner einlesen ---
                if (current_path.find("dataset") != std::string::npos) {
                    row.is_file_selector = true;
                    // Scannt nur nach Unterordnern im "data"-Verzeichnis
                    row.file_options = ScanDirectory("data", true); 
                    
                    std::string current_val = j.get<std::string>();
                    auto it = std::find(row.file_options.begin(), row.file_options.end(), current_val);
                    if (it != row.file_options.end()) row.current_file_idx = std::distance(row.file_options.begin(), it);
                }
                // Config Profile JSONs einlesen
                else if (current_path.find("config_profile") != std::string::npos) {
                    row.is_file_selector = true;
                    row.file_options = ScanDirectory("configs", false);
                    
                    std::string current_val = j.get<std::string>();
                    auto it = std::find(row.file_options.begin(), row.file_options.end(), current_val);
                    if (it != row.file_options.end()) row.current_file_idx = std::distance(row.file_options.begin(), it);
                }
            }
            grouped_rows[current_category][current_subcategory].push_back(row);
        }
    }

    void LoadConfigSmart(bool keep_cursors = false) {
        bool needs_save = false;
        
        std::ifstream file(config_path);
        if (file.is_open()) {
            try { 
                config_data = json::parse(file); 
                if (config_data.contains("error")) throw std::runtime_error("corrupt json");
            } 
            catch (...) { 
                config_data = CreateDefaultConfig(); 
                needs_save = true;
            }
            file.close();
        } else {
            config_data = CreateDefaultConfig();
            needs_save = true;
        }

        if (config_data.contains("backtest")) {
            if (config_data["backtest"].value("config_profile", "") != config_path) {
                config_data["backtest"]["config_profile"] = config_path;
            }
        }

        if (needs_save) {
            std::ofstream out(config_path);
            if (out.is_open()) out << config_data.dump(2);
        }
        
        categories.clear();
        subcategories.clear();
        grouped_rows.clear();
        FlattenJson(config_data, "", 0, "", "");

        for (auto& [cat, sub_map] : grouped_rows) {
            categories.push_back(cat);
            for (auto& [sub, rows] : sub_map) {
                subcategories[cat].push_back(sub);
            }
            auto& subs = subcategories[cat];
            auto it = std::find(subs.begin(), subs.end(), "General");
            if (it != subs.end() && it != subs.begin()) std::rotate(subs.begin(), it, it + 1);
        }
        
        if (!keep_cursors) {
            category_cursor = 0;
            subcat_cursor = 0;
            param_cursor = 0;
            menu_state = MenuState::CATEGORIES;
        } else {
            if (category_cursor >= categories.size()) category_cursor = 0;
            if (!categories.empty()) {
                auto& subs = subcategories[categories[category_cursor]];
                if (subcat_cursor >= subs.size()) subcat_cursor = 0;
                if (!subs.empty()) {
                    auto& rows = grouped_rows[categories[category_cursor]][subs[subcat_cursor]];
                    if (param_cursor >= rows.size()) param_cursor = 0;
                }
            }
        }
    }

    void SaveAndReload() {
        {
            std::ofstream file(config_path);
            if (file.is_open()) {
                file << config_data.dump(2);
                file.close(); 
            }
        }
        LoadConfigSmart(true); 
    }

public:
    std::string GetActiveConfigPath() const { return config_path; }

    PageConfig() { 
        fs::create_directories("configs"); 
        fs::create_directories("data"); // Stellt sicher, dass das HFT-Data-Dir existiert
        LoadConfigSmart(); 
    }
    
    bool BlocksGlobalHotkeys() const override {
        return menu_state == MenuState::INLINE_EDIT || menu_state == MenuState::SUBMENU_EDIT;
    }

    void OnEnter() override { 
        LoadConfigSmart(true); 
    }

    void Render(struct ncplane* stdplane) override {
        unsigned int dimy, dimx;
        ncplane_dim_yx(stdplane, &dimy, &dimx);

        int start_y = 11; 

        if (categories.empty()) {
            UITheme::StyleAlert(stdplane);
            ncplane_putstr_yx(stdplane, start_y, 3, "ERR: NO EDITABLE PARAMS.");
            return;
        }

        int visible_rows = std::max(5, (int)dimy - start_y - 4);

        if (category_cursor >= categories.size()) category_cursor = 0;
        
        for (size_t i = 0; i < categories.size() && (int)i < visible_rows; ++i) {
            int y = start_y + i;
            bool is_active_cat = (static_cast<int>(i) == category_cursor);

            if (is_active_cat && menu_state == MenuState::CATEGORIES) {
                UITheme::StyleCursorActive(stdplane);
                ncplane_putstr_yx(stdplane, y, 2, ">");
            } else {
                UITheme::StyleTextMuted(stdplane);
                ncplane_putstr_yx(stdplane, y, 2, " ");
            }

            if (is_active_cat) UITheme::StyleDataValue(stdplane);
            else UITheme::StyleTextDefault(stdplane);
            
            std::string cat_name = categories[i];
            std::transform(cat_name.begin(), cat_name.end(), cat_name.begin(), ::toupper);
            ncplane_putstr_yx(stdplane, y, 4, cat_name.c_str());
        }

        UITheme::StyleTextMuted(stdplane);
        for (int i = 0; i < visible_rows; ++i) ncplane_putstr_yx(stdplane, start_y + i, 21, "|");

        std::string current_cat = categories[category_cursor];
        auto& subcats = subcategories[current_cat];
        
        if (subcat_cursor >= subcats.size()) subcat_cursor = 0; 
        
        int sub_max_scroll = std::max(0, static_cast<int>(subcats.size()) - visible_rows);
        int sub_start_idx = std::max(0, std::min(subcat_cursor - visible_rows / 2, sub_max_scroll));

        for(int i = 0; i < visible_rows && (sub_start_idx + i) < static_cast<int>(subcats.size()); ++i) {
            int row_idx = sub_start_idx + i;
            int y = start_y + i;
            bool is_active_sub = (row_idx == subcat_cursor);
            std::string sub_name = subcats[row_idx];
            
            bool has_active_flag = false;
            bool is_module_active = false;
            for (const auto& row : grouped_rows[current_cat][sub_name]) {
                if (row.display_path == "active" || row.display_path == "enabled") {
                    has_active_flag = true;
                    if (config_data.contains(row.ptr) && config_data[row.ptr].is_boolean()) {
                        is_module_active = config_data[row.ptr].get<bool>();
                    }
                    break;
                }
            }

            if (is_active_sub && (menu_state == MenuState::SUBCATEGORIES || menu_state >= MenuState::PARAMETERS)) {
                UITheme::StyleCursorActive(stdplane);
                ncplane_putstr_yx(stdplane, y, 23, ">");
            } else {
                UITheme::StyleTextMuted(stdplane);
                ncplane_putstr_yx(stdplane, y, 23, " ");
            }

            if (is_active_sub) {
                UITheme::StyleDataValue(stdplane);
            } else if (has_active_flag) {
                if (is_module_active) UITheme::StyleDataValue(stdplane);
                else UITheme::StyleTextMuted(stdplane); 
            } else {
                UITheme::StyleTextDefault(stdplane); 
            }
            
            ncplane_putstr_yx(stdplane, y, 25, sub_name.c_str());
        }

        UITheme::StyleTextMuted(stdplane);
        for (int i = 0; i < visible_rows; ++i) ncplane_putstr_yx(stdplane, start_y + i, 43, "|");

        if (!subcats.empty()) {
            std::string current_sub = subcats[subcat_cursor];
            auto& rows = grouped_rows[current_cat][current_sub];
            
            if (param_cursor >= rows.size()) param_cursor = 0; 
            
            if (menu_state <= MenuState::INLINE_EDIT) {
                int max_scroll = std::max(0, static_cast<int>(rows.size()) - visible_rows);
                int start_idx = std::max(0, std::min(param_cursor - visible_rows / 2, max_scroll));

                for(int i = 0; i < visible_rows && (start_idx + i) < static_cast<int>(rows.size()); ++i) {
                    int row_idx = start_idx + i;
                    auto& row = rows[row_idx];
                    int y = start_y + i;
                    bool is_active_param = (row_idx == param_cursor);
                    
                    if (is_active_param && (menu_state == MenuState::PARAMETERS || menu_state == MenuState::INLINE_EDIT)) {
                        UITheme::StyleCursorActive(stdplane);
                        ncplane_putstr_yx(stdplane, y, 45, ">");
                    } else {
                        UITheme::StyleTextMuted(stdplane);
                        ncplane_putstr_yx(stdplane, y, 45, " ");
                    }
                    
                    ncplane_putstr_yx(stdplane, y, 47, row.display_path.c_str());
                    
                    std::string val_str;
                    if (is_active_param && menu_state == MenuState::INLINE_EDIT) {
                        if (row.is_file_selector) {
                            val_str = "< " + row.file_options[row.current_file_idx] + " >";
                            UITheme::StyleDataValue(stdplane); UITheme::StyleEditBackground(stdplane);
                        } else {
                            val_str = "[" + edit_buffer + "_]";
                            UITheme::StyleTextDefault(stdplane); UITheme::StyleEditBackground(stdplane);
                        }
                    } else {
                        if (row.is_bool) val_str = config_data[row.ptr].get<bool>() ? "[X] TRUE" : "[ ] FALSE";
                        else if (row.is_number) { 
                            if (row.is_range) {
                                double r_min = config_data[row.ptr].value("min", 0.0);
                                double r_max = config_data[row.ptr].value("max", 0.0);
                                double r_step = config_data[row.ptr].value("step", 0.0);
                                char buf[128]; snprintf(buf, sizeof(buf), "[GRID] %g : %g : %g", r_min, r_max, r_step);
                                val_str = buf;
                            } else {
                                val_str = DoubleToStr(config_data[row.ptr].get<double>());
                            }
                        }
                        else if (row.is_string) {
                            if (row.is_file_selector) val_str = "< " + row.file_options[row.current_file_idx] + " >";
                            else val_str = "\"" + config_data[row.ptr].get<std::string>() + "\"";
                        }
                        else val_str = "UNSUPPORTED";
                        
                        if (is_active_param && (menu_state == MenuState::PARAMETERS || menu_state == MenuState::INLINE_EDIT)) UITheme::StyleDataValue(stdplane);
                        else UITheme::StyleTextDefault(stdplane);
                    }
                    
                    ncplane_putstr_yx(stdplane, y, 75, val_str.c_str());
                    UITheme::StyleBackground(stdplane);
                }
            } 
            else if (menu_state == MenuState::SUBMENU_NAV || menu_state == MenuState::SUBMENU_EDIT) {
                auto& row = rows[param_cursor];
                
                UITheme::StyleDataValue(stdplane);
                ncplane_putstr_yx(stdplane, start_y, 45, ">>> GRID CONFIGURATOR <<<");
                
                UITheme::StyleTextDefault(stdplane);
                ncplane_putstr_yx(stdplane, start_y + 1, 45, ("TARGET: " + row.display_path).c_str());

                auto draw_field = [&](int cur_idx, int y, const std::string& label, const std::string& val) {
                    if (sub_state.cursor_y == cur_idx) UITheme::StyleCursorActive(stdplane);
                    else UITheme::StyleTextMuted(stdplane);
                    ncplane_putstr_yx(stdplane, y, 47, label.c_str());

                    if (sub_state.cursor_y == cur_idx && menu_state == MenuState::SUBMENU_EDIT) {
                        UITheme::StyleTextDefault(stdplane); UITheme::StyleEditBackground(stdplane);
                        ncplane_putstr_yx(stdplane, y, 65, ("[" + edit_buffer + "_]").c_str());
                    } else {
                        if (sub_state.cursor_y == cur_idx) UITheme::StyleDataValue(stdplane);
                        else UITheme::StyleTextDefault(stdplane);
                        ncplane_putstr_yx(stdplane, y, 65, val.c_str());
                    }
                    UITheme::StyleBackground(stdplane);
                };

                if (sub_state.cursor_y == 0) UITheme::StyleCursorActive(stdplane); else UITheme::StyleTextMuted(stdplane);
                ncplane_putstr_yx(stdplane, start_y + 3, 47, "Type:");
                if (sub_state.cursor_y == 0) UITheme::StyleDataValue(stdplane); else UITheme::StyleTextDefault(stdplane);
                ncplane_putstr_yx(stdplane, start_y + 3, 65, sub_state.is_range ? "< RANGE >" : "< STATIC >");

                if (sub_state.is_range) {
                    draw_field(1, start_y + 5, "Min:",  "[" + sub_state.min_str + "]");
                    draw_field(2, start_y + 6, "Max:",  "[" + sub_state.max_str + "]");
                    draw_field(3, start_y + 7, "Step:", "[" + sub_state.step_str + "]");
                    draw_field(4, start_y + 9, "", "[ SAVE ]");
                    draw_field(5, start_y + 10, "", "[ CANCEL ]");
                } else {
                    draw_field(1, start_y + 5, "Val:", "[" + sub_state.val_str + "]");
                    draw_field(2, start_y + 7, "", "[ SAVE ]");
                    draw_field(3, start_y + 8, "", "[ CANCEL ]");
                }
            }
        }

        std::string hline = std::string(dimx > 6 ? dimx - 6 : 10, '=');
        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, dimy - 3, 3, hline.c_str());
        
        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, dimy - 2, 3, "[ARROWS] NAVIGATE PANELS");
        ncplane_putstr_yx(stdplane, dimy - 2, 35, "[ENTER] EDIT/TOGGLE");
    }

    void HandleInput(uint32_t key) override {
        if (categories.empty()) return;
        
        if (category_cursor >= categories.size()) category_cursor = 0;
        std::string current_cat = categories[category_cursor];
        auto& subcats = subcategories[current_cat];
        
        if (subcat_cursor >= subcats.size()) subcat_cursor = 0;
        std::string current_sub = subcats.empty() ? "" : subcats[subcat_cursor];
        auto& rows = grouped_rows[current_cat][current_sub];
        if (param_cursor >= rows.size()) param_cursor = 0;

        if (menu_state == MenuState::CATEGORIES) {
            if (key == NCKEY_UP && category_cursor > 0) {
                category_cursor--;
                subcat_cursor = 0; 
                param_cursor = 0;  
            }
            else if (key == NCKEY_DOWN && category_cursor < static_cast<int>(categories.size()) - 1) {
                category_cursor++;
                subcat_cursor = 0; 
                param_cursor = 0;  
            }
            else if (key == NCKEY_RIGHT || key == NCKEY_ENTER) {
                menu_state = MenuState::SUBCATEGORIES;
                subcat_cursor = 0;
            }
        } 
        else if (menu_state == MenuState::SUBCATEGORIES) {
            if (subcats.empty()) return;
            if (key == NCKEY_UP && subcat_cursor > 0) {
                subcat_cursor--;
                param_cursor = 0; 
            }
            else if (key == NCKEY_DOWN && subcat_cursor < static_cast<int>(subcats.size()) - 1) {
                subcat_cursor++;
                param_cursor = 0; 
            }
            else if (key == NCKEY_LEFT) {
                menu_state = MenuState::CATEGORIES;
            }
            else if (key == NCKEY_RIGHT || key == NCKEY_ENTER) {
                menu_state = MenuState::PARAMETERS;
                param_cursor = 0;
            }
        }
        else if (menu_state == MenuState::PARAMETERS) {
            if (rows.empty()) return;
            auto& row = rows[param_cursor];

            if (key == NCKEY_UP && param_cursor > 0) param_cursor--;
            else if (key == NCKEY_DOWN && param_cursor < static_cast<int>(rows.size()) - 1) param_cursor++;
            else if (key == NCKEY_LEFT) menu_state = MenuState::SUBCATEGORIES;
            else if (key == NCKEY_ENTER) {
                if (row.is_bool) {
                    config_data[row.ptr] = !config_data[row.ptr].get<bool>();
                    SaveAndReload(); 
                } else if (row.is_number) {
                    menu_state = MenuState::SUBMENU_NAV;
                    sub_state.cursor_y = 0;
                    if (row.is_range) {
                        sub_state.is_range = true;
                        sub_state.min_str = DoubleToStr(config_data[row.ptr].value("min", 0.0));
                        sub_state.max_str = DoubleToStr(config_data[row.ptr].value("max", 0.0));
                        sub_state.step_str = DoubleToStr(config_data[row.ptr].value("step", 0.0));
                        sub_state.val_str = sub_state.min_str;
                    } else {
                        sub_state.is_range = false;
                        sub_state.val_str = DoubleToStr(config_data[row.ptr].get<double>());
                        sub_state.min_str = sub_state.val_str; 
                        sub_state.max_str = sub_state.val_str;
                        sub_state.step_str = "1.0";
                    }
                } else {
                    menu_state = MenuState::INLINE_EDIT;
                    if (!row.is_file_selector) edit_buffer = config_data[row.ptr].get<std::string>();
                }
            }
        }
        else if (menu_state == MenuState::INLINE_EDIT) {
            auto& row = rows[param_cursor];
            if (row.is_file_selector) {
                if (key == NCKEY_LEFT && row.current_file_idx > 0) row.current_file_idx--;
                else if (key == NCKEY_RIGHT && row.current_file_idx < static_cast<int>(row.file_options.size()) - 1) row.current_file_idx++;
                else if (key == NCKEY_ENTER) {
                    std::string selected_file = row.file_options[row.current_file_idx];
                    config_data[row.ptr] = selected_file;
                    
                    if (row.display_path == "config_profile") {
                        {
                            std::ofstream out(config_path);
                            if (out.is_open()) out << config_data.dump(2);
                        }
                        config_path = selected_file;
                        LoadConfigSmart(false);
                    } else {
                        SaveAndReload();
                        menu_state = MenuState::PARAMETERS;
                    }
                }
            } else {
                if (key == NCKEY_ENTER) {
                    config_data[row.ptr] = edit_buffer;
                    SaveAndReload();
                    menu_state = MenuState::PARAMETERS;
                }
                else if (key == NCKEY_BACKSPACE && !edit_buffer.empty()) edit_buffer.pop_back();
                else if (key >= 32 && key <= 126) edit_buffer += static_cast<char>(key);
            }
        }
        else if (menu_state == MenuState::SUBMENU_NAV) {
            int max_cur = sub_state.is_range ? 5 : 3;
            if (key == NCKEY_UP && sub_state.cursor_y > 0) sub_state.cursor_y--;
            else if (key == NCKEY_DOWN && sub_state.cursor_y < max_cur) sub_state.cursor_y++;
            else if (sub_state.cursor_y == 0 && (key == NCKEY_LEFT || key == NCKEY_RIGHT || key == NCKEY_ENTER)) {
                sub_state.is_range = !sub_state.is_range;
                if (sub_state.cursor_y > (sub_state.is_range ? 5 : 3)) sub_state.cursor_y = sub_state.is_range ? 5 : 3;
            }
            else if (key == NCKEY_ENTER) {
                if (sub_state.is_range && sub_state.cursor_y >= 1 && sub_state.cursor_y <= 3) {
                    menu_state = MenuState::SUBMENU_EDIT;
                    if (sub_state.cursor_y == 1) edit_buffer = sub_state.min_str;
                    if (sub_state.cursor_y == 2) edit_buffer = sub_state.max_str;
                    if (sub_state.cursor_y == 3) edit_buffer = sub_state.step_str;
                }
                else if (!sub_state.is_range && sub_state.cursor_y == 1) {
                    menu_state = MenuState::SUBMENU_EDIT;
                    edit_buffer = sub_state.val_str;
                }
                else if ((sub_state.is_range && sub_state.cursor_y == 4) || (!sub_state.is_range && sub_state.cursor_y == 2)) {
                    auto& row = rows[param_cursor];
                    if (sub_state.is_range) {
                        json r_obj;
                        r_obj["mode"] = "range";
                        r_obj["min"] = ParseDouble(sub_state.min_str);
                        r_obj["max"] = ParseDouble(sub_state.max_str);
                        r_obj["step"] = ParseDouble(sub_state.step_str);
                        r_obj["val"] = r_obj["min"];
                        config_data[row.ptr] = r_obj;
                    } else {
                        config_data[row.ptr] = ParseDouble(sub_state.val_str);
                    }
                    SaveAndReload();
                    menu_state = MenuState::PARAMETERS;
                }
                else if ((sub_state.is_range && sub_state.cursor_y == 5) || (!sub_state.is_range && sub_state.cursor_y == 3)) {
                    menu_state = MenuState::PARAMETERS;
                }
            }
        }
        else if (menu_state == MenuState::SUBMENU_EDIT) {
            if (key == NCKEY_ENTER) {
                if (sub_state.is_range) {
                    if (sub_state.cursor_y == 1) sub_state.min_str = edit_buffer;
                    if (sub_state.cursor_y == 2) sub_state.max_str = edit_buffer;
                    if (sub_state.cursor_y == 3) sub_state.step_str = edit_buffer;
                } else {
                    if (sub_state.cursor_y == 1) sub_state.val_str = edit_buffer;
                }
                menu_state = MenuState::SUBMENU_NAV;
            }
            else if (key == NCKEY_BACKSPACE && !edit_buffer.empty()) edit_buffer.pop_back();
            else if (key >= 32 && key <= 126) edit_buffer += static_cast<char>(key);
        }
    }
};
