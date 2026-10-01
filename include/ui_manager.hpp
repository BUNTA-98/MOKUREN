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
#include "ui_tradelog.hpp"  
#include "ui_footprint.hpp" 
#include "ui_equity.hpp"

// reordered for a clean left-to-right workflow (config -> scan -> results)
enum class PageID { CONFIG, SCANNER, EQUITY, TRADELOG, INSPECTOR };

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
    
        // main top header rendering
        int hdr_origin_y = 1;
        std::string hline = std::string(dimx > 6 ? dimx - 6 : 10, '=');
        std::string header_L1 = "---//O-O\\\\---- // <MOKUREN MFT ENGINE> //";
        std::string header_L2 = "--||O-O-O||-- //         V0.5         //";
        std::string header_L3 = "---\\\\O-O//-- //                      //";
        std::string usr_id= " >>> USR_ID  <BUNTA>"; 
            
        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, hdr_origin_y + 1, 3, header_L1.c_str());
        ncplane_putstr_yx(stdplane, hdr_origin_y, 3, hline.c_str());
          
        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, hdr_origin_y + 2, 3, header_L2.c_str());
        ncplane_putstr_yx(stdplane, hdr_origin_y + 3, 3, header_L3.c_str());
        UITheme::StyleCursorActive(stdplane);
        ncplane_putstr_yx(stdplane, hdr_origin_y + 3, 29, usr_id.c_str());
        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, hdr_origin_y + 4, 3, hline.c_str());
          
        UITheme::StyleTextDefault(stdplane);
        std::string title = (current_page_id == PageID::CONFIG) ? "VIEW: CONFIG MATRIX" : 
                            (current_page_id == PageID::SCANNER) ? "VIEW: GRID SCANNER" : 
                            (current_page_id == PageID::EQUITY) ? "VIEW: EQUITY CURVE" : 
                            (current_page_id == PageID::TRADELOG) ? "VIEW: TRADE JOURNAL" : "VIEW: FOOTPRINT";
                                  
        int title_x = dimx > (title.length() + 5) ? dimx - title.length() - 5 : 60;
        ncplane_putstr_yx(stdplane, hdr_origin_y + 2, title_x, title.c_str());
            
        // dynamic page navigation bar (left-to-right workflow)
        int pg_bar_y = hdr_origin_y + 5;
        (current_page_id == PageID::CONFIG) ? UITheme::StyleDataValue(stdplane) : UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, pg_bar_y, 3, "[1] CONFIG");
            
        (current_page_id == PageID::SCANNER) ? UITheme::StyleDataValue(stdplane) : UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, pg_bar_y, 18, "[2] SCANNER");

        (current_page_id == PageID::EQUITY) ? UITheme::StyleDataValue(stdplane) : UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, pg_bar_y, 34, "[3] EQUITY");
            
        (current_page_id == PageID::TRADELOG) ? UITheme::StyleDataValue(stdplane) : UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, pg_bar_y, 49, "[4] TRADELOG");
            
        (current_page_id == PageID::INSPECTOR) ? UITheme::StyleDataValue(stdplane) : UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, pg_bar_y, 66, "[5] FOOTPRINT");

        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, pg_bar_y + 1, 3, hline.c_str());
            
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
                [this](const UIResult& winning_run) { 
                    
                    std::string data_path = "binance/monthly/DEFAULT.csv"; 
                    
                    std::function<void(const nlohmann::json&)> find_path = [&](const nlohmann::json& node) {
                        if (node.is_object()) {
                            for (auto& [k, v] : node.items()) {
                                if (v.is_string() && (k == "filepath" || k == "data_path")) {
                                    data_path = v.get<std::string>(); 
                                } else { find_path(v); }
                            }
                        } else if (node.is_array()) {
                            for (auto& item : node) find_path(item);
                        }
                    };
                    find_path(winning_run.full_config);

                    // automatic wfa stitching or standard grid replay
                    if (winning_run.params_str.find("WFA_Mode") != std::string::npos && !winning_run.wfa_configs.empty()) {
                        this->last_replay = this->engine.ReplayWFA(data_path, winning_run.wfa_configs);
                    } else {
                        this->last_replay = this->engine.ReplaySingleRun(data_path, winning_run.full_config);
                    }
                    
                    // auto-route directly to the trade journal for detailed drill-down
                    this->SwitchPage(PageID::TRADELOG);
                }
            ); 
        }
        else if (new_page == PageID::TRADELOG) {
            auto tlog = std::make_unique<UITradelog>([this](size_t target_candle) {
                this->SwitchPage(PageID::INSPECTOR);
                if (auto* insp = dynamic_cast<UIInspector*>(this->current_page.get())) {
                    // insp->JumpToCandle(target_candle); // <--- UNCOMMENT AFTER FOOTPRINT UPDATE
                }
            });
            tlog->SetData(last_replay.trades, last_replay.history_1m);
            current_page = std::move(tlog);
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

            // apply new hotkey mapping
            if (!is_editing && (key == 'q' || key == 'Q')) running = false;
            else if (!is_editing && key == '1') SwitchPage(PageID::CONFIG);
            else if (!is_editing && key == '2') SwitchPage(PageID::SCANNER);
            else if (!is_editing && key == '3') SwitchPage(PageID::EQUITY);
            else if (!is_editing && key == '4') SwitchPage(PageID::TRADELOG);
            else if (!is_editing && key == '5') SwitchPage(PageID::INSPECTOR);
            else if (!is_editing && (key == NCKEY_TAB || key == '\t')) { 
                int next_id = (static_cast<int>(current_page_id) + 1) % 5;
                SwitchPage(static_cast<PageID>(next_id));
            }
            else {
                if (current_page) current_page->HandleInput(key);
            }
        }
    }
};