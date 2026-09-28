#pragma once
#include <notcurses/notcurses.h>
#include <stdexcept>
#include <memory>
#include <string>
#include "engine_state.hpp"
#include "mokuren.hpp"

#include "ui_theme.hpp"
#include "ui_pages.hpp"
#include "ui_menu.hpp"
#include "ui_scanner.hpp"   
#include "ui_footprint.hpp" 
#include "ui_equity.hpp"

enum class PageID { INSPECTOR, EQUITY, SCANNER, CONFIG };

class UIManager {
private:
    struct notcurses* nc;
    struct ncplane* stdplane;
    
    EngineState state;
    Mokuren engine;
    ReplayResult last_replay; 
    
    PageID current_page_id = PageID::CONFIG; 
    std::unique_ptr<UIPage> current_page;

    void DrawGlobalHeader() {
        unsigned int dimy, dimx;
        ncplane_dim_yx(stdplane, &dimy, &dimx);


        // dynamic separator line spanning full width
        std::string hline = std::string(dimx > 6 ? dimx - 6 : 10, '=');

        UITheme::StyleAlert(stdplane);
        ncplane_putstr_yx(stdplane, 5, 3, ">>>"); // shifted start to y=5
        
        
        UITheme::StyleTextDefault(stdplane);
        std::string title = (current_page_id == PageID::CONFIG) ? "VIEW: CONFIG MATRIX" : 
                            (current_page_id == PageID::SCANNER) ? "VIEW: GRID SCANNER" : 
                            (current_page_id == PageID::EQUITY) ? "VIEW: EQUITY CURVE" : "VIEW: FOOTPRINT";
        
        // anchor title to the right side safely
        int title_x = dimx > (title.length() + 5) ? dimx - title.length() - 5 : 60;
        ncplane_putstr_yx(stdplane, 5, title_x, title.c_str());

        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 7, 3, hline.c_str());
        
        (current_page_id == PageID::INSPECTOR) ? UITheme::StyleDataValue(stdplane) : UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 8, 3, "[1] INSPECTOR");
        
        (current_page_id == PageID::EQUITY) ? UITheme::StyleDataValue(stdplane) : UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 8, 20, "[2] EQUITY");
        
        (current_page_id == PageID::SCANNER) ? UITheme::StyleDataValue(stdplane) : UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 8, 35, "[3] SCANNER");
        
        (current_page_id == PageID::CONFIG) ? UITheme::StyleDataValue(stdplane) : UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 8, 50, "[4] CONFIG");

        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 9, 3, hline.c_str());
        
        // anchor global footer to bottom right (y = dimy - 1)
        int footer_x = dimx > 40 ? dimx - 35 : 5;
        ncplane_putstr_yx(stdplane, dimy - 1, footer_x, "[Q] DISCONNECT   [TAB] NEXT VIEW");
    }

    void SwitchPage(PageID new_page) {
        current_page_id = new_page;
        
        if (new_page == PageID::CONFIG) {
            current_page = std::make_unique<PageConfig>(); 
        } 
        else if (new_page == PageID::SCANNER) { 
            current_page = std::make_unique<UIScanner>(&state, &engine, 
                [this](const nlohmann::json& winning_config) {
                    std::string data_path = "binance/monthly/DEFAULT.csv"; 
                    try {
                        std::ifstream f("config.json");
                        if (f.is_open()) {
                            nlohmann::json j = nlohmann::json::parse(f);
                            std::function<void(const nlohmann::json&)> find_path = [&](const nlohmann::json& node) {
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

                    this->last_replay = this->engine.ReplaySingleRun(data_path, winning_config);
                    this->SwitchPage(PageID::EQUITY);
                }
            ); 
        }
        else if (new_page == PageID::EQUITY) {
            auto equity = std::make_unique<UIEquityCurve>();
            equity->SetReplayData(last_replay.trades);
            current_page = std::move(equity);
        }
        else if (new_page == PageID::INSPECTOR) { 
            auto inspector = std::make_unique<UIInspector>();
            inspector->SetReplayData(last_replay.history_1m, last_replay.history_15m, last_replay.trades);
            current_page = std::move(inspector);
        }
        
        if (current_page) current_page->OnEnter();
    }

public:
    UIManager() {
        notcurses_options opts = {0};
        opts.flags = NCOPTION_SUPPRESS_BANNERS; 
        nc = notcurses_core_init(&opts, nullptr);
        if (!nc) throw std::runtime_error("fatal error: arasaka core init failed.");
        stdplane = notcurses_stdplane(nc);
        
        SwitchPage(PageID::CONFIG);
    }

    ~UIManager() {
        if (nc) notcurses_stop(nc);
    }

    void Run() {
        bool running = true;
        while (running) {
            ncplane_erase(stdplane);
            UITheme::StyleBackground(stdplane);

            DrawGlobalHeader();
            if (current_page) current_page->Render(stdplane);
            
            notcurses_render(nc);

            struct timespec ts = {0, 16000000}; 
            ncinput ni;
            uint32_t key = notcurses_get(nc, &ts, &ni);
            
            if (key == (uint32_t)-1 || ni.evtype == NCTYPE_RELEASE) continue; 

            bool is_editing = current_page && current_page->BlocksGlobalHotkeys();

            if (!is_editing && (key == 'q' || key == 'Q')) running = false;
            else if (!is_editing && key == '1') SwitchPage(PageID::INSPECTOR);
            else if (!is_editing && key == '2') SwitchPage(PageID::EQUITY);
            else if (!is_editing && key == '3') SwitchPage(PageID::SCANNER);
            else if (!is_editing && key == '4') SwitchPage(PageID::CONFIG);
            else if (!is_editing && (key == NCKEY_TAB || key == '\t')) { 
                int next_id = (static_cast<int>(current_page_id) + 1) % 4;
                SwitchPage(static_cast<PageID>(next_id));
            }
            else {
                if (current_page) current_page->HandleInput(key);
            }
        }
    }
};
