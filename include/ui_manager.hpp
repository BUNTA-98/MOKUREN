#pragma once
#include <notcurses/notcurses.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <vector>
#include <nlohmann/json.hpp> // JSON Bibliothek
#include "engine_state.hpp"
#include "mokuren.hpp"

using json = nlohmann::json;

// --- PAGE STATES ---
enum class Page {
  INSPECTOR,
  SCANNER,
  CONFIG
};

// --- DATA STRUCTURE FOR SMART MENU ---
struct MenuRow {
  std::string display_path;
  json::json_pointer ptr;
  bool is_bool;
  bool is_number;
  bool is_string;
};

class UIManager {
private:
  struct notcurses* nc;
  struct ncplane* stdplane;
  
  EngineState state;
  Mokuren engine;
  
  Page current_page = Page::SCANNER; 

  // --- CONFIG EDITOR STATE ---
  json config_data;
  std::vector<MenuRow> menu_rows;
  int config_cursor = 0;
  bool config_edit_mode = false;
  std::string edit_buffer = "";
  std::string config_path = "config.json";

  // --- ARASAKA / MOKUREN COLOR PALETTE ---
  void set_color_arasaka_red() { ncplane_set_fg_rgb8(stdplane, 230, 10, 20); }
  void set_color_mokuren_pink() { ncplane_set_fg_rgb8(stdplane, 255, 50, 150); }
  void set_color_data_cyan() { ncplane_set_fg_rgb8(stdplane, 0, 200, 255); }
  void set_color_dim_gray() { ncplane_set_fg_rgb8(stdplane, 80, 80, 80); }
  void set_color_white() { ncplane_set_fg_rgb8(stdplane, 240, 240, 240); }
  void set_color_bg() { ncplane_set_bg_rgb8(stdplane, 8, 8, 10); }

  // REKURSIVE FUNKTION: Wandelt JSON in flache UI-Reihen um
  void FlattenJson(const json& j, const std::string& current_path) {
    if (j.is_object()) {
      for (auto& [key, val] : j.items()) {
        FlattenJson(val, current_path + "/" + key);
      }
    } else if (j.is_array()) {
      for (size_t i = 0; i < j.size(); ++i) {
        FlattenJson(j[i], current_path + "/" + std::to_string(i));
      }
    } else {
      // Das ist ein Endknoten (Zahl, Text oder Bool)
      MenuRow row;
      std::string display = current_path;
      if (display.empty()) display = "ROOT";
      if (display[0] == '/') display = display.substr(1);
      std::replace(display.begin(), display.end(), '/', '.'); // Optik: Slashes zu Punkten machen
      
      row.display_path = display;
      row.ptr = json::json_pointer(current_path);
      row.is_bool = j.is_boolean();
      row.is_number = j.is_number();
      row.is_string = j.is_string();
      menu_rows.push_back(row);
    }
  }

  void LoadConfigSmart() {
    std::ifstream file(config_path);
    if (file.is_open()) {
      try {
        config_data = json::parse(file);
      } catch (...) {
        config_data = json::object();
        config_data["error"] = "Invalid JSON Parsing";
      }
    } else {
      config_data = json::object();
    }
    
    menu_rows.clear();
    FlattenJson(config_data, "");
    if (config_cursor >= static_cast<int>(menu_rows.size())) config_cursor = 0;
  }

  void SaveConfigSmart() {
    std::ofstream file(config_path);
    if (file.is_open()) {
      file << config_data.dump(2); // Speichert mit 2 Leerzeichen Einrückung
    }
  }

  // --- UNIVERSAL HEADER ---
  void DrawHeader(const std::string& page_title) {
    set_color_arasaka_red();
    ncplane_putstr_yx(stdplane, 1, 3, ">>>");
    
    set_color_mokuren_pink();
    ncplane_putstr_yx(stdplane, 1, 7, "ARASAKA CORP. // MOKUREN NEURAL ENGINE // [SECURE]");
    
    set_color_white();
    ncplane_putstr_yx(stdplane, 1, 60, page_title.c_str());

    set_color_dim_gray();
    ncplane_putstr_yx(stdplane, 3, 3, "========================================================================================");
    
    (current_page == Page::INSPECTOR) ? set_color_data_cyan() : set_color_dim_gray();
    ncplane_putstr_yx(stdplane, 4, 3, "[1] INSPECTOR");
    
    (current_page == Page::SCANNER) ? set_color_data_cyan() : set_color_dim_gray();
    ncplane_putstr_yx(stdplane, 4, 20, "[2] GRID SCANNER");
    
    (current_page == Page::CONFIG) ? set_color_data_cyan() : set_color_dim_gray();
    ncplane_putstr_yx(stdplane, 4, 40, "[3] CONFIG MATRIX");

    set_color_dim_gray();
    ncplane_putstr_yx(stdplane, 5, 3, "========================================================================================");
  }

  void DrawInspector() {
    DrawHeader("VIEW: K-FOOTPRINT INSPECTOR");
    set_color_white();
    ncplane_putstr_yx(stdplane, 10, 3, "[ ORDERFLOW RENDER ZONE ]");
    set_color_dim_gray();
    ncplane_putstr_yx(stdplane, 12, 3, "Awaiting Footprint Data Stream...");
    ncplane_putstr_yx(stdplane, 27, 3, "[Q] DISCONNECT");
  }

  void DrawScanner() {
    DrawHeader("VIEW: GRID SCANNER");
    
    std::string current_status = state.GetStatus();
    int current_perm = state.current_permutation.load();
    int total_perm = state.total_permutations.load();
    bool is_running = state.is_running.load();

    set_color_white();
    ncplane_putstr_yx(stdplane, 7, 3, "[ SYSTEM DIAGNOSTICS ]");
    
    set_color_dim_gray();
    ncplane_putstr_yx(stdplane, 9, 3, "CORE_LINK     :");
    set_color_data_cyan();
    ncplane_putstr_yx(stdplane, 9, 19, "ESTABLISHED");
    
    set_color_dim_gray();
    ncplane_putstr_yx(stdplane, 10, 3, "WORKER_THREAD :");
    if (is_running) {
        set_color_mokuren_pink();
        ncplane_putstr_yx(stdplane, 10, 19, "ACTIVE");
    } else {
        set_color_dim_gray();
        ncplane_putstr_yx(stdplane, 10, 19, "SLEEPING");
    }

    set_color_white();
    ncplane_putstr_yx(stdplane, 7, 40, "[ HYPERPARAMETER MATRIX ]");
    
    set_color_dim_gray();
    ncplane_putstr_yx(stdplane, 9, 40, "STATUS:");
    set_color_arasaka_red();
    ncplane_putstr_yx(stdplane, 9, 48, current_status.c_str());

    set_color_dim_gray();
    ncplane_putstr_yx(stdplane, 11, 40, "PERMUTATIONS :");
    set_color_white();
    std::string perm_text = std::to_string(current_perm) + " / " + std::to_string(total_perm);
    ncplane_putstr_yx(stdplane, 11, 55, perm_text.c_str());

    set_color_dim_gray();
    ncplane_putstr_yx(stdplane, 13, 40, "PROGRESS:");
    
    int bar_width = 40;
    int filled = total_perm > 0 ? (current_perm * bar_width) / total_perm : 0;
    std::string bar = "[";
    for(int i = 0; i < bar_width; i++) bar += (i < filled) ? "#" : ".";
    bar += "]";
    
    if (is_running) set_color_mokuren_pink();
    else set_color_dim_gray();
    ncplane_putstr_yx(stdplane, 14, 40, bar.c_str());

    set_color_white();
    ncplane_putstr_yx(stdplane, 17, 3, "[ LIVE LEADERBOARD : TOP 5 ]");
    
    set_color_dim_gray();
    ncplane_putstr_yx(stdplane, 19, 3, "RANK  PROFIT       WINRATE   TRADES   PARAMETERS");
    ncplane_putstr_yx(stdplane, 20, 3, "--------------------------------------------------------------------------------");

    std::vector<UIResult> top_runs;
    {
      std::lock_guard<std::mutex> lock(state.ui_mutex);
      top_runs = state.top_results;
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
               
      if (i == 0) set_color_data_cyan(); else set_color_white();
      ncplane_putstr_yx(stdplane, 21 + i, 3, buf);
    }

    if (!is_running) set_color_arasaka_red(); else set_color_dim_gray();
    ncplane_putstr_yx(stdplane, 27, 3, "[S] INITIATE OVERRIDE");
    set_color_dim_gray();
    ncplane_putstr_yx(stdplane, 27, 30, "[Q] DISCONNECT");
  }

  void DrawConfig() {
    DrawHeader("VIEW: SMART CONFIG MATRIX");
    
    int start_y = 8;
    int visible_rows = 17;
    
    if (menu_rows.empty()) {
        set_color_arasaka_red();
        ncplane_putstr_yx(stdplane, start_y, 3, "ERROR: No editable parameters found.");
        return;
    }
    
    // Auto-Scroll Logik
    int max_scroll = std::max(0, static_cast<int>(menu_rows.size()) - visible_rows);
    int start_idx = std::max(0, std::min(config_cursor - visible_rows / 2, max_scroll));
    
    for(int i = 0; i < visible_rows && (start_idx + i) < static_cast<int>(menu_rows.size()); ++i) {
        int row_idx = start_idx + i;
        auto& row = menu_rows[row_idx];
        int y = start_y + i;
        
        bool is_active = (row_idx == config_cursor);
        
        if (is_active) {
            set_color_mokuren_pink();
            ncplane_putstr_yx(stdplane, y, 3, ">");
        } else {
            set_color_dim_gray();
            ncplane_putstr_yx(stdplane, y, 3, " ");
        }
        
        // Parameter-Pfad (z.B. strategy.sl_pct)
        ncplane_putstr_yx(stdplane, y, 6, row.display_path.c_str());
        
        // Werte zeichnen
        std::string val_str;
        if (is_active && config_edit_mode) {
            val_str = "[" + edit_buffer + "_]";
            set_color_white();
            ncplane_set_bg_rgb8(stdplane, 150, 10, 30);
        } else {
            if (row.is_bool) {
                val_str = config_data[row.ptr].get<bool>() ? "[X] TRUE" : "[ ] FALSE";
            }
            else if (row.is_number) {
                double d = config_data[row.ptr].get<double>();
                char buf[64];
                snprintf(buf, sizeof(buf), "%g", d);
                val_str = std::string(buf);
            }
            else if (row.is_string) {
                val_str = "\"" + config_data[row.ptr].get<std::string>() + "\"";
            }
            else val_str = "UNSUPPORTED";
            
            if (is_active) set_color_data_cyan();
            else set_color_white();
        }
        
        ncplane_putstr_yx(stdplane, y, 50, val_str.c_str());
        set_color_bg();
    }

    set_color_dim_gray();
    ncplane_putstr_yx(stdplane, 26, 3, "========================================================================================");
    set_color_white();
    ncplane_putstr_yx(stdplane, 27, 3, "[UP/DOWN] NAVIGATE");
    ncplane_putstr_yx(stdplane, 27, 25, "[ENTER] EDIT/TOGGLE");
    set_color_dim_gray();
    ncplane_putstr_yx(stdplane, 27, 50, "[Q] DISCONNECT");
  }

public:
  UIManager() {
    notcurses_options opts = {0};
    opts.flags = NCOPTION_SUPPRESS_BANNERS; 
    nc = notcurses_core_init(&opts, nullptr);
    if (!nc) throw std::runtime_error("fatal error: arasaka core init failed.");
    stdplane = notcurses_stdplane(nc);
    
    LoadConfigSmart();
  }

  ~UIManager() {
    if (nc) notcurses_stop(nc);
  }

  void Run() {
    bool running = true;
    
    while (running) {
      ncplane_erase(stdplane);
      set_color_bg();

      switch (current_page) {
        case Page::INSPECTOR: DrawInspector(); break;
        case Page::SCANNER:   DrawScanner();   break;
        case Page::CONFIG:    DrawConfig();    break;
      }
      
      notcurses_render(nc);

      struct timespec ts = {0, 16000000}; 
      ncinput ni;
      uint32_t key = notcurses_get(nc, &ts, &ni);
      
      if (key == (uint32_t)-1) continue; 
      if (ni.evtype == NCTYPE_RELEASE) continue; 

      // --- GLOBALE HOTKEYS ---
      if (key == 'q' || key == 'Q') running = false;
      if (key == '1') current_page = Page::INSPECTOR;
      if (key == '2') {
          current_page = Page::SCANNER;
          LoadConfigSmart(); // Lädt aktualisierte Daten, falls Page 3 was geändert hat
      }
      if (key == '3') {
          current_page = Page::CONFIG;
          LoadConfigSmart(); 
      }
      
      // --- PAGE 2: SCANNER LOGIC ---
      if (current_page == Page::SCANNER) {
        if ((key == 's' || key == 'S') && !state.is_running.load()) {
          state.is_running = true;
          state.current_permutation = 0;
          state.total_permutations = 0;
          {
              std::lock_guard<std::mutex> lock(state.ui_mutex);
              state.top_results.clear();
          }

          std::thread([this]() {
              std::string data_path = "binance/monthly"; 
              engine.RunGridSearch(data_path, config_path, 
                  [this](const std::string& status) { state.SetStatus(status); },
                  [this](int current, int total) { state.current_permutation = current; state.total_permutations = total; },
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
                      
                      std::lock_guard<std::mutex> lock(state.ui_mutex);
                      state.top_results.push_back(ur);
                  }
              );
              state.is_running = false;
              state.SetStatus("IDLE");
          }).detach();
        }
      }

      // --- PAGE 3: SMART CONFIG LOGIC ---
      if (current_page == Page::CONFIG) {
        if (menu_rows.empty()) continue; // Nichts zu steuern

        if (key == NCKEY_ENTER) {
          if (!config_edit_mode) {
            auto& row = menu_rows[config_cursor];
            if (row.is_bool) {
              // Booleans (Checkboxes) sofort umkehren und speichern
              bool val = config_data[row.ptr].get<bool>();
              config_data[row.ptr] = !val;
              SaveConfigSmart();
            } else {
              // Bei Text und Nummern in den roten Edit-Modus wechseln
              config_edit_mode = true;
              if (row.is_number) {
                  double val = config_data[row.ptr].get<double>();
                  char buf[64]; snprintf(buf, sizeof(buf), "%g", val);
                  edit_buffer = buf;
              } else if (row.is_string) {
                  edit_buffer = config_data[row.ptr].get<std::string>();
              }
            }
          } else {
            // Edit-Modus bestätigen und speichern
            config_edit_mode = false;
            auto& row = menu_rows[config_cursor];
            try {
              if (row.is_number) config_data[row.ptr] = std::stod(edit_buffer);
              else if (row.is_string) config_data[row.ptr] = edit_buffer;
              SaveConfigSmart();
            } catch (...) {
              // Ungültige Eingabe wird ignoriert
            }
          }
        } 
        else if (config_edit_mode) {
          // Im roten Edit-Modus Werte abtippen
          if (key == NCKEY_BACKSPACE && !edit_buffer.empty()) {
            edit_buffer.pop_back();
          } else if (key >= 32 && key <= 126) {
            edit_buffer += static_cast<char>(key);
          }
        } 
        else {
          // Normale Menü-Navigation (Hoch / Runter)
          if (key == NCKEY_UP && config_cursor > 0) {
            config_cursor--;
          }
          else if (key == NCKEY_DOWN && config_cursor < static_cast<int>(menu_rows.size()) - 1) {
            config_cursor++;
          }
        }
      }
    }
  }
};