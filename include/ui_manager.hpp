#pragma once
#include <notcurses/notcurses.h>
#include <stdexcept>
#include <memory>
#include <string>
#include "engine_state.hpp"
#include "mokuren.hpp"
#include "log_ring.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

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
    
    // hold active config path across page switches. empty until the config
    // page reports its selection, so we never silently fall back to a
    // hardcoded profile.
    std::string active_config_path;

    bool      show_log   = false;
    int       log_scroll = 0;    // 0 = follow the newest line
    uint64_t  log_seq    = 0;
    std::vector<mokuren::LogEntry> log_view;
    static constexpr size_t kLogViewMax = 3000;

    int capture_cycle = 0;   // 0=info 1=debug 2=trace
    int view_filter   = 0;   // 0=all 1=warn+ 2=err

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

    static spdlog::level::level_enum CaptureLevel(int cycle) {
        switch (cycle) {
            case 1:  return spdlog::level::debug;
            case 2:  return spdlog::level::trace;
            default: return spdlog::level::info;
        }
    }

    void PullLog() {
        auto fresh = mokuren::GetLogRing().SnapshotAfter(log_seq);
        if (fresh.empty()) return;
        log_seq = fresh.back().seq;
        // pause-on-scroll: when the operator has scrolled away from the tail,
        // anchor the view to the entry they were looking at instead of letting
        // new arrivals push it toward the newest line. log_scroll is measured
        // from the newest entry, so a stable "N from newest" drifts as `total`
        // grows; capturing the seq of the top visible line and re-deriving
        // log_scroll after the append keeps the same line under the cursor.
        uint64_t anchor_seq = 0;
        const bool anchored = (log_scroll > 0) && !log_view.empty();
        if (anchored) {
            const int idx = (int)log_view.size() - 1 - log_scroll;
            if (idx >= 0 && idx < (int)log_view.size()) {
                anchor_seq = log_view[idx].seq;
            }
        }
        for (auto& e : fresh) log_view.push_back(std::move(e));
        if (log_view.size() > kLogViewMax)
            log_view.erase(log_view.begin(),
                           log_view.begin() + (log_view.size() - kLogViewMax));
        if (anchored && anchor_seq != 0) {
            // find the anchor again; if it was trimmed out of the ring, fall
            // back to the oldest surviving entry so the view does not jump.
            int new_idx = -1;
            for (int i = (int)log_view.size() - 1; i >= 0; --i) {
                if (log_view[i].seq == anchor_seq) { new_idx = i; break; }
            }
            if (new_idx < 0) new_idx = 0;
            log_scroll = (int)log_view.size() - 1 - new_idx;
        }
        const int max_scroll = std::max(0, (int)log_view.size() - 1);
        if (log_scroll > max_scroll) log_scroll = max_scroll;
    }

    static void StyleLogLevel(spdlog::level::level_enum lvl, struct ncplane* p) {
        if (lvl >= spdlog::level::err) {
            ncplane_set_fg_rgb8(p, 240, 240, 240);
            ncplane_set_bg_rgb8(p, 150, 10, 30);      // white on dark red
        } else if (lvl == spdlog::level::warn) {
            UITheme::StyleAlert(p);
            ncplane_set_bg_default(p);
        } else if (lvl <= spdlog::level::debug) {
            UITheme::StyleTextMuted(p);
        } else {
            UITheme::StyleTextDefault(p);
        }
    }

    void DrawLogPanel() {
        unsigned int dimy = 0, dimx = 0;
        ncplane_dim_yx(stdplane, &dimy, &dimx);
        // a terminal shorter than this cannot fit the panel plus the global
        // footer without y0 going negative and the panel escaping the plane.
        // suppressing it is cheaper than debugging an ncplane overrun.
        if ((int)dimy < 12) return;
        const int w  = std::min<int>((int)dimx - 6, 200);
        const int h  = std::max<int>(8, (int)dimy * 2 / 5);
        const int x0 = ((int)dimx - w) / 2;
        // dimy-2 keeps the global [Q] footer at the very last row visible.
        const int y0 = (int)dimy - 2 - h;

        UITheme::StyleBackground(stdplane);
        for (int i = 0; i < h; ++i)
            ncplane_putstr_yx(stdplane, y0 + i, x0, std::string(w, ' ').c_str());

        UITheme::StyleDataValue(stdplane);
        ncplane_putstr_yx(stdplane, y0, x0, std::string(w, '=').c_str());
        ncplane_putstr_yx(stdplane, y0 + h - 1, x0, std::string(w, '=').c_str());
        for (int i = 1; i < h - 1; ++i) {
            ncplane_putstr_yx(stdplane, y0 + i, x0, "|");
            ncplane_putstr_yx(stdplane, y0 + i, x0 + w - 1, "|");
        }

        auto passes = [&](const mokuren::LogEntry& e) {
            if (view_filter == 1 && e.level < spdlog::level::warn) return false;
            if (view_filter == 2 && e.level < spdlog::level::err)  return false;
            return true;
        };

        const int total = (int)log_view.size();
        const int body_y = y0 + 2;
        const int body_h = h - 4;
        const int body_x = x0 + 2;
        const int body_w = w - 4;
        const char* mode = (log_scroll == 0) ? "FOLLOW" : "SCROLLED";
        static const char* kCap[] = {"info", "debug", "trace"};
        static const char* kView[] = {"all", "warn+", "err"};

        char title[192];
        std::snprintf(title, sizeof(title),
            "[ LOG ] %d lines  cap=%s  view=%s  %s",
            total, kCap[capture_cycle], kView[view_filter], mode);
        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, y0 + 1, x0 + 2, title);

        // left carries *state*, right carries *actions*. splitting them
        // means the state stays visible on a narrow terminal: a legend
        // that would collide with the title is dropped, not wrapped.
        // wrapping would push the body down and make the panel shift
        // height every time `cap=` or `view=` changed width.
        static const char* kHelp =
            "[UP/DN]SCROLL  [PG]PAGE  [HOME/END]JUMP  "
            "[F]CAPTURE  [V]VIEW  [L/ESC]CLOSE";
        const int help_len  = (int)std::strlen(kHelp);
        const int title_len = (int)std::strlen(title);
        if (w - 4 > title_len + help_len + 4) {
            UITheme::StyleTextMuted(stdplane);
            ncplane_putstr_yx(stdplane, y0 + 1, x0 + w - 2 - help_len, kHelp);
        }

        if (body_h <= 0 || body_w <= 2 || total == 0) {
            UITheme::StyleTextMuted(stdplane);
            ncplane_putstr_yx(stdplane, body_y, body_x,
                              "(no log output yet -- start a scan, then [F] for debug)");
            return;
        }

        // walk backwards collecting the visible (filtered) entries we need.
        // filtering at draw time, not ingest, so toggling [V] recovers lines
        // that were already pulled into log_view.
        std::vector<int> vis;
        vis.reserve((size_t)body_h + (size_t)log_scroll);
        for (int i = total - 1; i >= 0 && (int)vis.size() < body_h + log_scroll; --i) {
            if (passes(log_view[i])) vis.push_back(i);
        }
        const int total_vis = (int)vis.size();
        // clamp log_scroll against the *filtered* count, not the raw ring
        // size. with [V]=warn+ and 1000 info lines / 3 warnings, the arrow
        // clamp in PullLog let log_scroll climb to 999 while only 3 lines
        // were visible, so many keypresses looked dead before anything moved.
        // the display was always correct (k >= total_vis skips), this just
        // makes the keys respond at the first press.
        if (log_scroll > std::max(0, total_vis - 1)) {
            log_scroll = std::max(0, total_vis - 1);
        }

        for (int row = 0; row < body_h; ++row) {
            const int k = log_scroll + (body_h - 1 - row);
            if (k >= total_vis) continue;
            const auto& e = log_view[vis[k]];
            StyleLogLevel(e.level, stdplane);
            std::string s = e.text;
            if ((int)s.size() > body_w) s = s.substr(0, body_w - 3) + "...";
            ncplane_putstr_yx(stdplane, body_y + row, body_x, s.c_str());
        }

        if (total_vis > body_h) {
            const int sb_h    = body_h;
            const int thumb   = std::max(1, sb_h * body_h / total_vis);
            const int denom   = std::max(1, total_vis - body_h);
            const int pos     = std::min(total_vis - body_h, log_scroll);
            // log_scroll is measured from the newest line (0 = follow), so
            // convert to a top-relative offset before mapping onto the bar.
            // the old expression collapsed to `pos`, which put the thumb at
            // the top in follow mode -- exactly inverted.
            const int pos_top = (total_vis - body_h) - pos;
            const int tpos    = (sb_h - thumb) * pos_top / denom;
            UITheme::StyleTextMuted(stdplane);
            for (int i = 0; i < sb_h; ++i) {
                const bool on = i >= tpos && i < tpos + thumb;
                ncplane_putstr_yx(stdplane, body_y + i, x0 + w - 2, on ? "#" : ".");
            }
        }
    }

    void OpenLog() {
        show_log = true;
        log_scroll = 0;
        mokuren::SetLogRingLevel(CaptureLevel(capture_cycle));
    }

    void CloseLog() {
        show_log = false;
        log_scroll = 0;
        capture_cycle = 0;
        // the ring survives (reopening replays what was already captured), but
        // per-signal debug format cost is paid only while the panel is visible.
        // a closed panel with the ring left at debug is the exact state this
        // was meant to avoid.
        mokuren::SetLogRingLevel(spdlog::level::info);
    }

    void SwitchPage(PageID new_page) {
        // capture active config before config page is destroyed. this is the
        // single source of truth for which profile the scanner/backtest runs.
        if (current_page_id == PageID::CONFIG && current_page) {
            if (auto* p_config = dynamic_cast<PageConfig*>(current_page.get())) {
                active_config_path = p_config->GetActiveConfigPath();
            }
        }

        current_page_id = new_page;
        
        if (new_page == PageID::CONFIG) {
            auto p_config = std::make_unique<PageConfig>();
            // restore the previously selected profile so navigating back from
            // the scanner does not silently reset to configs/default.json.
            // if no profile was ever selected, fall back to the constructor's
            // default (configs/default.json) rather than leaving it unset.
            if (!active_config_path.empty()) {
                p_config->SetActiveConfigPath(active_config_path);
            } else {
                active_config_path = p_config->GetActiveConfigPath();
            }
            current_page = std::move(p_config);
        } 
        else if (new_page == PageID::SCANNER) { 
            // pass the captured path to the scanner. if it is empty the config
            // page was never visited, which is a programming error rather than
            // a reason to silently load a default profile.
            if (active_config_path.empty()) {
                throw std::runtime_error("scanner launched without an active config profile");
            }
            current_page = std::make_unique<UIScanner>(&state, &engine, active_config_path, 
                [this](const UIResult& winning_run) { 
                    
                    // data path is resolved from the winning run's config. no
                    // hardcoded fallback: if the config carries no dataset we
                    // surface the error rather than replaying the wrong file.
                    // one key, one owner: environment.dataset (with filepath
                    // as the legacy alias). the old recursive last-wins scan
                    // could pick up a "dataset" key from anywhere in the tree
                    // and silently replay the wrong file.
                    std::string data_path;
                    if (winning_run.full_config.contains("environment") &&
                        winning_run.full_config["environment"].is_object()) {
                        const auto& env = winning_run.full_config["environment"];
                        data_path = env.value("dataset", env.value("filepath", std::string{}));
                    }
                    if (data_path.empty()) {
                        throw std::runtime_error("winning run has no environment.dataset in its config");
                    }

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
            PullLog();

            ncplane_erase(stdplane);
            UITheme::StyleBackground(stdplane);

            DrawGlobalHeader();
            if (current_page) current_page->Render(stdplane);

            if (show_log) DrawLogPanel();

            notcurses_render(nc);

            struct timespec ts = {0, 16000000}; 
            ncinput ni;
            uint32_t key = notcurses_get(nc, &ts, &ni);
            
            if (key == (uint32_t)-1 || ni.evtype == NCTYPE_RELEASE) continue; 

            bool is_editing = current_page && current_page->BlocksGlobalHotkeys();

            // apply new hotkey mapping
            if (!is_editing && (key == 'l' || key == 'L')) {
                show_log ? CloseLog() : OpenLog();
            }
            else if (show_log && !is_editing && (key == NCKEY_ESC)) {
                CloseLog();
            }
            else if (show_log && !is_editing && (key == NCKEY_UP || key == NCKEY_PGUP)) {
                const int step = (key == NCKEY_PGUP) ? 20 : 1;
                log_scroll = std::min(log_scroll + step,
                                      std::max(0, (int)log_view.size() - 1));
            }
            else if (show_log && !is_editing && (key == NCKEY_DOWN || key == NCKEY_PGDOWN)) {
                const int step = (key == NCKEY_PGDOWN) ? 20 : 1;
                log_scroll = std::max(0, log_scroll - step);
            }
            else if (show_log && !is_editing && key == NCKEY_HOME) {
                log_scroll = std::max(0, (int)log_view.size() - 1);
            }
            else if (show_log && !is_editing && key == NCKEY_END) {
                log_scroll = 0;
            }
            else if (show_log && !is_editing && (key == 'f' || key == 'F')) {
                capture_cycle = (capture_cycle + 1) % 3;
                mokuren::SetLogRingLevel(CaptureLevel(capture_cycle));
            }
            else if (show_log && !is_editing && (key == 'v' || key == 'V')) {
                view_filter = (view_filter + 1) % 3;
            }
            else if (!is_editing && (key == 'q' || key == 'Q')) running = false;
            else if (!is_editing && key == '1') SwitchPage(PageID::CONFIG);
            else if (!is_editing && key == '2') SwitchPage(PageID::SCANNER);
            else if (!is_editing && key == '3') SwitchPage(PageID::EQUITY);
            else if (!is_editing && key == '4') SwitchPage(PageID::TRADELOG);
            else if (!is_editing && key == '5') SwitchPage(PageID::INSPECTOR);
            else if (!is_editing && (key == NCKEY_TAB || key == '\t')) { 
                int next_id = (static_cast<int>(current_page_id) + 1) % 5;
                SwitchPage(static_cast<PageID>(next_id));
            }
            // --- NEU: Fängt S und W im Config Menü ab und routet sie durch ---
            // capture the selected profile *before* switching pages so the
            // scanner is constructed with the exact config the user picked.
            else if (!is_editing && (key == 's' || key == 'S' || key == 'w' || key == 'W') && current_page_id == PageID::CONFIG) {
                if (current_page) {
                    if (auto* p_config = dynamic_cast<PageConfig*>(current_page.get())) {
                        active_config_path = p_config->GetActiveConfigPath();
                    }
                }
                SwitchPage(PageID::SCANNER);
                // arm the scanner's one-shot autostart, then deliver the key.
                // the scanner consumes the flag inside HandleInput so the scan
                // is initialized exactly once even if OnEnter() also ran.
                if (auto* scanner = dynamic_cast<UIScanner*>(current_page.get())) {
                    scanner->ArmAutostart(key);
                }
                if (current_page) current_page->HandleInput(key);
            }
            // ----------------------------------------------------------------
            else {
                if (current_page) current_page->HandleInput(key);
            }
        }
    }
};
