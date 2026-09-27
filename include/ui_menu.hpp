#pragma once
#include "ui_pages.hpp"
#include "ui_theme.hpp"
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

enum class MenuState {
    CATEGORIES,     // Linke Spalte
    PARAMETERS,     // Rechte Spalte (Liste)
    INLINE_EDIT,    // String/File bearbeiten
    SUBMENU_NAV,    // Im Parameter-Configurator navigieren
    SUBMENU_EDIT    // Zahl im Configurator eintippen
};

struct MenuRow {
    std::string display_path;
    json::json_pointer ptr;
    bool is_bool = false;
    bool is_number = false;
    bool is_string = false;
    bool is_range = false; // NEU: Ist es ein Grid-Objekt?

    bool is_file_selector = false;
    std::vector<std::string> file_options;
    int current_file_idx = 0;
};

// Temporärer Speicher für unser Untermenü
struct ParamConfigState {
    bool is_range = false;
    std::string val_str = "";
    std::string min_str = "";
    std::string max_str = "";
    std::string step_str = "";
    int cursor_y = 0; // 0=Type, 1=Val/Min, 2=Max, 3=Step, 4=Save, 5=Cancel
};

class PageConfig : public UIPage {
private:
    json config_data;
    
    std::vector<std::string> categories;
    std::map<std::string, std::vector<MenuRow>> grouped_rows;
    
    MenuState menu_state = MenuState::CATEGORIES;
    int category_cursor = 0;
    int param_cursor = 0;
    
    std::string edit_buffer = "";
    std::string config_path = "config.json";
    
    ParamConfigState sub_state;

    // Helper: Double zu String ohne ewig viele Nullen
    std::string DoubleToStr(double d) {
        char buf[64]; snprintf(buf, sizeof(buf), "%g", d);
        return std::string(buf);
    }
    
    // Helper: Safely parse double
    double ParseDouble(const std::string& str) {
        try { return std::stod(str); } catch (...) { return 0.0; }
    }

    std::vector<std::string> ScanDirectory(const std::string& folder) {
        std::vector<std::string> files;
        try {
            if (fs::exists(folder)) {
                for (const auto& entry : fs::recursive_directory_iterator(folder)) {
                    if (entry.is_regular_file() && entry.path().extension() == ".csv") {
                        files.push_back(entry.path().string());
                    }
                }
            }
        } catch (...) {}
        
        if (files.empty()) files.push_back("NO_FILES_FOUND");
        return files;
    }

    void FlattenJson(const json& j, const std::string& current_path, const std::string& current_category) {
        if (j.is_object()) {
            // NEU: Erkennt unser Grid-Range Objekt
            if (j.contains("mode") && j["mode"] == "range" || 
                (j.contains("min") && j.contains("max") && j.contains("step"))) {
                MenuRow row;
                std::string display = current_path.empty() ? "ROOT" : current_path.substr(current_category.length() + 2);
                std::replace(display.begin(), display.end(), '/', '.');
                if (display.empty()) display = "value";

                row.display_path = display;
                row.ptr = json::json_pointer(current_path);
                row.is_number = true;
                row.is_range = true; // Markieren als Range!
                grouped_rows[current_category].push_back(row);
            } else {
                // Normales rekursives Weitergehen
                for (auto& [key, val] : j.items()) {
                    std::string next_cat = current_category.empty() ? key : current_category;
                    FlattenJson(val, current_path + "/" + key, next_cat);
                }
            }
        } else if (j.is_array()) {
            for (size_t i = 0; i < j.size(); ++i) {
                FlattenJson(j[i], current_path + "/" + std::to_string(i), current_category);
            }
        } else {
            MenuRow row;
            std::string display = current_path.empty() ? "ROOT" : current_path.substr(current_category.length() + 2);
            std::replace(display.begin(), display.end(), '/', '.');
            if (display.empty()) display = "value";

            row.display_path = display;
            row.ptr = json::json_pointer(current_path);
            row.is_bool = j.is_boolean();
            row.is_number = j.is_number();
            
            if (j.is_string()) {
                row.is_string = true;
                if (current_path.find("filepath") != std::string::npos || current_path.find("data_path") != std::string::npos) {
                    row.is_file_selector = true;
                    row.file_options = ScanDirectory("binance");
                    
                    std::string current_val = j.get<std::string>();
                    auto it = std::find(row.file_options.begin(), row.file_options.end(), current_val);
                    if (it != row.file_options.end()) {
                        row.current_file_idx = std::distance(row.file_options.begin(), it);
                    }
                }
            }
            grouped_rows[current_category].push_back(row);
        }
    }

    void LoadConfigSmart() {
        std::ifstream file(config_path);
        if (file.is_open()) {
            try { config_data = json::parse(file); } 
            catch (...) { config_data = json::object(); config_data["error"] = "Invalid JSON Parsing"; }
        } else {
            config_data = json::object();
        }
        
        categories.clear();
        grouped_rows.clear();
        FlattenJson(config_data, "", "");

        if (config_data.is_object()) {
            for (auto& [key, val] : config_data.items()) {
                if (grouped_rows.count(key)) categories.push_back(key);
            }
        }
        
        if (category_cursor >= static_cast<int>(categories.size())) category_cursor = 0;
        param_cursor = 0;
        menu_state = MenuState::CATEGORIES;
    }

    void SaveConfigSmart() {
        std::ofstream file(config_path);
        if (file.is_open()) file << config_data.dump(2);
    }

public:
    PageConfig() { LoadConfigSmart(); }
    
  bool BlocksGlobalHotkeys() const override {
        return menu_state == MenuState::INLINE_EDIT || menu_state == MenuState::SUBMENU_EDIT;
    }

    void OnEnter() override { LoadConfigSmart(); }

    void Render(struct ncplane* stdplane) override {
        int start_y = 8;
        
        if (categories.empty()) {
            UITheme::StyleAlert(stdplane);
            ncplane_putstr_yx(stdplane, start_y, 3, "ERROR: No editable parameters found.");
            return;
        }

        // --- LINKE SPALTE: KATEGORIEN ---
        for (size_t i = 0; i < categories.size(); ++i) {
            int y = start_y + i;
            bool is_active_cat = (static_cast<int>(i) == category_cursor);

            if (is_active_cat && menu_state == MenuState::CATEGORIES) {
                UITheme::StyleCursorActive(stdplane);
                ncplane_putstr_yx(stdplane, y, 3, ">");
            } else {
                UITheme::StyleTextMuted(stdplane);
                ncplane_putstr_yx(stdplane, y, 3, " ");
            }

            if (is_active_cat) UITheme::StyleDataValue(stdplane);
            else UITheme::StyleTextDefault(stdplane);
            
            std::string cat_name = categories[i];
            std::transform(cat_name.begin(), cat_name.end(), cat_name.begin(), ::toupper);
            ncplane_putstr_yx(stdplane, y, 6, cat_name.c_str());
        }

        // --- TRENNLINIE ---
        UITheme::StyleTextMuted(stdplane);
        for (int i = 0; i < 15; ++i) ncplane_putstr_yx(stdplane, start_y + i, 26, "|");

        // --- RECHTE SPALTE ---
        std::string current_cat = categories[category_cursor];
        auto& rows = grouped_rows[current_cat];
        
        if (menu_state == MenuState::CATEGORIES || menu_state == MenuState::PARAMETERS || menu_state == MenuState::INLINE_EDIT) {
            // NORMALE PARAMETER-LISTE
            int visible_rows = 15;
            int max_scroll = std::max(0, static_cast<int>(rows.size()) - visible_rows);
            int start_idx = std::max(0, std::min(param_cursor - visible_rows / 2, max_scroll));

            for(int i = 0; i < visible_rows && (start_idx + i) < static_cast<int>(rows.size()); ++i) {
                int row_idx = start_idx + i;
                auto& row = rows[row_idx];
                int y = start_y + i;
                bool is_active_param = (row_idx == param_cursor);
                
                if (is_active_param && (menu_state == MenuState::PARAMETERS || menu_state == MenuState::INLINE_EDIT)) {
                    UITheme::StyleCursorActive(stdplane);
                    ncplane_putstr_yx(stdplane, y, 30, ">");
                } else {
                    UITheme::StyleTextMuted(stdplane);
                    ncplane_putstr_yx(stdplane, y, 30, " ");
                }
                
                ncplane_putstr_yx(stdplane, y, 33, row.display_path.c_str());
                
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
                
                ncplane_putstr_yx(stdplane, y, 55, val_str.c_str());
                UITheme::StyleBackground(stdplane);
            }
        } 
        else if (menu_state == MenuState::SUBMENU_NAV || menu_state == MenuState::SUBMENU_EDIT) {
            // UNTERMENÜ FÜR CONFIG (SPLIT SCREEN TRANSFORM)
            auto& row = rows[param_cursor];
            
            UITheme::StyleDataValue(stdplane);
            ncplane_putstr_yx(stdplane, start_y, 30, ">>> PARAMETER CONFIGURATOR <<<");
            
            UITheme::StyleTextDefault(stdplane);
            ncplane_putstr_yx(stdplane, start_y + 1, 30, ("TARGET: " + row.display_path).c_str());

            auto draw_field = [&](int cur_idx, int y, const std::string& label, const std::string& val) {
                if (sub_state.cursor_y == cur_idx) UITheme::StyleCursorActive(stdplane);
                else UITheme::StyleTextMuted(stdplane);
                ncplane_putstr_yx(stdplane, y, 32, label.c_str());

                if (sub_state.cursor_y == cur_idx && menu_state == MenuState::SUBMENU_EDIT) {
                    UITheme::StyleTextDefault(stdplane); UITheme::StyleEditBackground(stdplane);
                    ncplane_putstr_yx(stdplane, y, 42, ("[" + edit_buffer + "_]").c_str());
                } else {
                    if (sub_state.cursor_y == cur_idx) UITheme::StyleDataValue(stdplane);
                    else UITheme::StyleTextDefault(stdplane);
                    ncplane_putstr_yx(stdplane, y, 42, val.c_str());
                }
                UITheme::StyleBackground(stdplane);
            };

            // TYPE SELECTION
            if (sub_state.cursor_y == 0) UITheme::StyleCursorActive(stdplane); else UITheme::StyleTextMuted(stdplane);
            ncplane_putstr_yx(stdplane, start_y + 3, 32, "Type:");
            if (sub_state.cursor_y == 0) UITheme::StyleDataValue(stdplane); else UITheme::StyleTextDefault(stdplane);
            ncplane_putstr_yx(stdplane, start_y + 3, 42, sub_state.is_range ? "< RANGE (GRID) >" : "< STATIC >");

            if (sub_state.is_range) {
                draw_field(1, start_y + 5, "Min:",  "[" + sub_state.min_str + "]");
                draw_field(2, start_y + 6, "Max:",  "[" + sub_state.max_str + "]");
                draw_field(3, start_y + 7, "Step:", "[" + sub_state.step_str + "]");
                
                draw_field(4, start_y + 9, "", "[ SAVE & RETURN ]");
                draw_field(5, start_y + 10, "", "[ CANCEL ]");
            } else {
                draw_field(1, start_y + 5, "Val:", "[" + sub_state.val_str + "]");
                
                draw_field(2, start_y + 7, "", "[ SAVE & RETURN ]");
                draw_field(3, start_y + 8, "", "[ CANCEL ]");
            }
        }

        // --- FOOTER ---
        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 26, 3, "========================================================================================");
        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, 27, 3, "[ARROWS] NAVIGATE PANELS");
        ncplane_putstr_yx(stdplane, 27, 30, "[ENTER] EDIT/TOGGLE");
    }

    void HandleInput(uint32_t key) override {
        if (categories.empty()) return;
        std::string current_cat = categories[category_cursor];
        auto& rows = grouped_rows[current_cat];

        if (menu_state == MenuState::CATEGORIES) {
            if (key == NCKEY_UP && category_cursor > 0) category_cursor--;
            else if (key == NCKEY_DOWN && category_cursor < static_cast<int>(categories.size()) - 1) category_cursor++;
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
            else if (key == NCKEY_LEFT) menu_state = MenuState::CATEGORIES;
            else if (key == NCKEY_ENTER) {
                if (row.is_bool) {
                    config_data[row.ptr] = !config_data[row.ptr].get<bool>();
                    SaveConfigSmart();
                    LoadConfigSmart();
                } else if (row.is_number) {
                    // ÖFFNE DAS UNTERMENÜ
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
                    config_data[row.ptr] = row.file_options[row.current_file_idx];
                    SaveConfigSmart();
                    menu_state = MenuState::PARAMETERS;
                }
            } else {
                if (key == NCKEY_ENTER) {
                    config_data[row.ptr] = edit_buffer;
                    SaveConfigSmart();
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
                    // SPEICHERN
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
                    SaveConfigSmart();
                    LoadConfigSmart(); // Rebuild structure
                    menu_state = MenuState::PARAMETERS;
                }
                else if ((sub_state.is_range && sub_state.cursor_y == 5) || (!sub_state.is_range && sub_state.cursor_y == 3)) {
                    // ABBRECHEN
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
