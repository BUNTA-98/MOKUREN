#pragma once
#include <notcurses/notcurses.h>
#include <stdexcept>
#include <memory>
#include "engine_state.hpp"
#include "mokuren.hpp"

#include "ui_theme.hpp"
#include "ui_pages.hpp"
#include "ui_menu.hpp"
#include "ui_scanner.hpp"   
#include "ui_footprint.hpp" 

enum class PageID { INSPECTOR, SCANNER, CONFIG };

class UIManager {
private:
    struct notcurses* nc;
    struct ncplane* stdplane;
    
    EngineState state;
    Mokuren engine;
    
    PageID current_page_id = PageID::CONFIG; 
    std::unique_ptr<UIPage> current_page;

    void DrawGlobalHeader() {
        UITheme::StyleAlert(stdplane);
        ncplane_putstr_yx(stdplane, 1, 3, ">>>");
        
        UITheme::StyleCursorActive(stdplane);
        ncplane_putstr_yx(stdplane, 1, 7, "ARASAKA CORP. // MOKUREN NEURAL ENGINE // [SECURE]");
        
        UITheme::StyleTextDefault(stdplane);
        std::string title = (current_page_id == PageID::CONFIG) ? "VIEW: SMART CONFIG MATRIX" : 
                            (current_page_id == PageID::SCANNER) ? "VIEW: GRID SCANNER" : "VIEW: K-FOOTPRINT INSPECTOR";
        ncplane_putstr_yx(stdplane, 1, 60, title.c_str());

        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 3, 3, "========================================================================================");
        
        (current_page_id == PageID::INSPECTOR) ? UITheme::StyleDataValue(stdplane) : UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 4, 3, "[1] INSPECTOR");
        
        (current_page_id == PageID::SCANNER) ? UITheme::StyleDataValue(stdplane) : UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 4, 20, "[2] GRID SCANNER");
        
        (current_page_id == PageID::CONFIG) ? UITheme::StyleDataValue(stdplane) : UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 4, 40, "[3] CONFIG MATRIX");

        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 5, 3, "========================================================================================");
        
        ncplane_putstr_yx(stdplane, 27, 50, "[Q] DISCONNECT");
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
                                        } else {
                                            find_path(v); 
                                        }
                                    }
                                }
                            };
                            find_path(j);
                        }
                    } catch (...) {}

                    ReplayResult rep = this->engine.ReplaySingleRun(data_path, winning_config);

                    auto inspector = std::make_unique<UIInspector>();
                    inspector->SetReplayData(rep.history_1m, rep.history_15m, rep.trades);
                    
                    this->current_page = std::move(inspector);
                    this->current_page_id = PageID::INSPECTOR;
                    if (this->current_page) this->current_page->OnEnter();
                }
            ); 
        }
        else if (new_page == PageID::INSPECTOR) { 
            current_page = std::make_unique<UIInspector>(); 
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

            // HIER IST DER FIX: Wir checken, ob die aktuelle Seite blockiert
            bool is_editing = current_page && current_page->BlocksGlobalHotkeys();

            if (!is_editing && (key == 'q' || key == 'Q')) running = false;
            else if (!is_editing && key == '1') SwitchPage(PageID::INSPECTOR);
            else if (!is_editing && key == '2') SwitchPage(PageID::SCANNER);
            else if (!is_editing && key == '3') SwitchPage(PageID::CONFIG);
            else {
                // Wenn wir tippen (is_editing == true), landet ALLES hier (auch die '1' und das 'Q')
                if (current_page) current_page->HandleInput(key);
            }
        }
    }
};