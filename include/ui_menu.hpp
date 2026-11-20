#pragma once
#include "component_schema.hpp"
#include "config_parser.hpp"
#include "config_schema.hpp"
#include "ui_pages.hpp"
#include "ui_theme.hpp"
#include <cstdint>
#include <cstdio>
#include <limits>
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
    SUBMENU_EDIT,
    PROFILE_PICKER, // modal profile picker (17.3). The row that used to switch
                    // profiles was backtest.config_profile, which D6 deletes.
};

// One row in the parameter panel.
//
// `def` is the table entry the row renders; it is null for a component-array
// row and for a file-driven row, whose shape still comes from the json value
// because the table deliberately does not model those (config.md section 5).
//
// The four type bools are gone. Each was computed once at flatten time from a
// value that can change under the row (a grid node edited back to a scalar),
// and each now has exactly one owner: `def->kind` for a schema row, the json
// itself for the other two.
struct MenuRow {
    const mokuren::cfg::Field* def = nullptr;
    json::json_pointer ptr;
    std::string display_path;
    bool is_selector = false;
    std::vector<std::string> options;
    int current_idx = 0;

    // Component rows are addressed by NAME, not by index. `ptr` above would be
    // /risk/modules/3/cooldown_ms, and it is only correct for as long as the
    // array keeps that exact order in both the buffer and the skeleton. Nothing
    // enforces that: a merge, an insertion, a reordered file, and the row
    // points at a slot that holds something else (or nothing) for the rest of
    // the session -- which is exactly how every module row came to render "??".
    //
    // The loader itself resolves components by `name` (StrategyFactory::Build,
    // BuildExitPlan), so the menu resolves them the same way and the index
    // stops being load-bearing.
    std::string component_path;  // dotted array path; empty for schema rows
    std::string component_name;  // the entry's `name`
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

    // The json as it was read, for the footer's [modified] marker: a
    // comparison, not a dirty flag that can drift away from the file.
    json loaded_data;

    // Profile picker state (17.3).
    std::vector<std::string> profile_options;
    int profile_cursor = 0;

    // Footer status. Both advisory -- AppConfig::Load stays the only gate.
    size_t unknown_keys = 0;
    std::string config_error;

    // The complete shape lives in the skeleton (component_schema.hpp): every
    // table field at its default and every catalogue component present with
    // enabled:false. `config_data` is ALWAYS that shape with the file laid over
    // it, which is what lets BuildMenu ignore the file entirely.
    json skeleton_data = mokuren::cfg::Skeleton();

    std::string DoubleToStr(double d) {
        char buf[64]; snprintf(buf, sizeof(buf), "%g", d);
        return std::string(buf);
    }
    
    double ParseDouble(const std::string& str) {
        try { return std::stod(str); } catch (...) { return 0.0; }
    }

    // --- table-driven helpers (P2) -----------------------------------------

    // The component arrays, taken from the catalogue rather than typed again --
    // one list of what the factory can build, not two that drift.
    static std::vector<std::string> ComponentArrayPaths() {
        std::vector<std::string> out;
        for (const mokuren::cfg::ComponentDef& d : mokuren::cfg::Catalogue()) {
            if (std::find(out.begin(), out.end(), d.array_path) == out.end()) {
                out.push_back(d.array_path);
            }
        }
        return out;
    }

    // Whole subtrees shown in the menu but not in the schema: `backtest` is
    // read by the WFA runner, not by AppConfig::Load (section 5), so it stays
    // out of v1 while its settings must not vanish from the UI.
    static constexpr const char* kFileDrivenSubtrees[] = {
        "backtest",
    };

    // A retired config key a profile may still carry. D6 deleted
    // backtest.config_profile: "which file am I editing" is UI state now, and
    // a row for it would write a key nothing reads.
    static bool IsRetiredKey(const std::string& dotted) {
        return dotted == "backtest.config_profile";
    }

    // Drop the retired keys from the buffer, so the next save writes a profile
    // that does not reference itself. Filtering the menu alone left the key in
    // the file forever: it is exempt from the unknown-key check (the `backtest`
    // subtree is allowed), so nothing else would ever remove it.
    static void DropRetiredKeys(json& j) {
        if (j.contains("backtest") && j["backtest"].is_object()) {
            j["backtest"].erase("config_profile");
        }
    }

    static std::string CategoryForComponent(const std::string& path) {
        if (path == "triggers") return "Triggers";
        if (path == "filters") return "Filters";
        if (path == "risk.modules") return "Risk";
        if (path == "trade_management.structural_exit.rules")
            return "Trade Management";
        if (path == "backtest") return "Backtest";
        return path;
    }

    // Category order = the smallest Field.order the category declares. A
    // category owning no schema field has no number to sort by, so it takes a
    // slot after the last schema section: components first, then file-driven
    // data. Not a second list of categories -- the only names here are the
    // three the schema cannot carry.
    static int CategoryOrder(const std::string& cat) {
        int best = std::numeric_limits<int>::max();
        for (const mokuren::cfg::Field& f : mokuren::cfg::Schema()) {
            if (cat == f.category && f.order < best) best = f.order;
        }
        if (best != std::numeric_limits<int>::max()) return best;
        if (cat == "Triggers") return 900;
        if (cat == "Filters") return 910;
        return std::numeric_limits<int>::max();
    }

    static int SubcategoryOrder(const std::string& cat, const std::string& sub) {
        int best = std::numeric_limits<int>::max();
        for (const mokuren::cfg::Field& f : mokuren::cfg::Schema()) {
            if (cat == f.category && sub == f.subcategory && f.order < best)
                best = f.order;
        }
        return best;
    }

    // A schema path is dotted ("risk.sl_pct"); a json pointer is not
    // ("/risk/sl_pct"). One owner for the mapping.
    static json::json_pointer SchemaPointer(const char* path) {
        std::string p(path);
        std::replace(p.begin(), p.end(), '.', '/');
        return json::json_pointer("/" + p);
    }

    // Point the picker at the current value, inserting it when the scan does
    // not list it: a hand-written path, or the "data/" default, is a value the
    // picker must show rather than silently replace on the next ENTER.
    static int SeedSelection(MenuRow& row, const std::string& current) {
        auto it = std::find(row.options.begin(), row.options.end(), current);
        if (it != row.options.end())
            return static_cast<int>(std::distance(row.options.begin(), it));
        if (current.empty()) return 0;
        row.options.insert(row.options.begin(), current);
        return 0;
    }

    // A row is "inherited" when its value equals the skeleton default, which is
    // exactly the condition under which Diff() omits it from the file. Same
    // predicate for a schema row and a component param, so the dimming means
    // one thing: this key is not in the profile.
    //
    // Display-only resolution: an inherited key shows its default and is drawn
    // dimmed. Nothing here writes (D4, config.md 10.2).
    json EffectiveValue(const MenuRow& row) {
        if (row.component_path.empty()) {
            const json* v = mokuren::cfg::Lookup(config_data, row.def->path);
            return v == nullptr ? row.def->default_value : *v;
        }

        // Component row: find the entry BY NAME in the array it names, then
        // read the param out of it. An index is not consulted, so a merge that
        // reorders or inserts cannot strand the row. If the entry is missing
        // (a profile that never had that component, or an array that arrived
        // after the menu was built), the catalogue default is the honest
        // answer -- the row shows what enabling it WOULD mean.
        const std::string param = row.ptr.back();
        const json* arr = mokuren::cfg::Lookup(config_data, row.component_path);
        if (arr != nullptr && arr->is_array()) {
            for (const json& e : *arr) {
                if (!e.is_object()) continue;
                if (e.value("name", std::string()) != row.component_name) continue;
                if (e.contains(param)) return e.at(param);
                break;
            }
        }
        if (const mokuren::cfg::ComponentDef* d =
                mokuren::cfg::FindComponent(row.component_path,
                                            row.component_name)) {
            if (d->params.contains(param)) return d->params.at(param);
        }
        return json();
    }

    bool IsAbsent(const MenuRow& row) const {
        // A component param is part of the entry, not a table default that can
        // be inherited -- the entry carries its value by construction. What is
        // dimmed for a component is the whole subcategory's enabled flag, which
        // the second panel already does.
        if (!row.component_path.empty()) return false;
        try {
            if (!skeleton_data.contains(row.ptr)) return false;
            return config_data.at(row.ptr) == skeleton_data.at(row.ptr);
        } catch (...) {
            return false;
        }
    }

    // Role of each row, derived at draw time rather than stored: the value can
    // change kind under a row without the row being rebuilt.
    bool RowIsBool(const MenuRow& row) {
        if (row.def != nullptr) return row.def->kind == mokuren::cfg::Kind::Bool;
        return EffectiveValue(row).is_boolean();
    }

    bool RowIsNumeric(const MenuRow& row) {
        if (row.def != nullptr) {
            return row.def->kind == mokuren::cfg::Kind::Int ||
                   row.def->kind == mokuren::cfg::Kind::Double;
        }
        const json v = EffectiveValue(row);
        return v.is_number() || mokuren::cfg::IsGridNode(v);
    }

    bool RowIsText(const MenuRow& row) {
        if (row.def != nullptr) return row.def->kind == mokuren::cfg::Kind::String;
        return EffectiveValue(row).is_string();
    }

    // One sweep predicate, the table's (section 9). Shape only, so a component
    // param is a grid here exactly like a schema scalar.
    bool RowIsGrid(const MenuRow& row) {
        return mokuren::cfg::IsGridNode(EffectiveValue(row));
    }

    std::string SelectedOption(const MenuRow& row) const {
        if (row.options.empty()) return "< nothing to pick >";
        const int idx = std::min(std::max(row.current_idx, 0),
                                 static_cast<int>(row.options.size()) - 1);
        return "< " + row.options[idx] + " >";
    }

    std::string FormatValue(const MenuRow& row) {
        const json v = EffectiveValue(row);
        if (RowIsBool(row)) {
            return (v.is_boolean() && v.get<bool>()) ? "[X] TRUE" : "[ ] FALSE";
        }
        if (RowIsGrid(row)) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "[%g->%g*%g=%g]",
                          v.value("min", 0.0), v.value("max", 0.0),
                          v.value("step", 0.0), v.value("val", 0.0));
            return std::string(buf);
        }
        if (row.is_selector) return SelectedOption(row);
        if (v.is_number()) return DoubleToStr(v.get<double>());
        if (v.is_string()) return "\"" + v.get<std::string>() + "\"";
        // "UNSUPPORTED" used to stand for four different things: a nested
        // object, an array, a null, and a pointer that does not resolve. The
        // first two are real values and are shown; the last two are a lookup
        // problem, and saying so is the difference between "this row is odd"
        // and "this row points at nothing".
        if (v.is_object() || v.is_array()) {
            std::string dumped = v.dump();
            if (dumped.size() > 40) dumped = dumped.substr(0, 37) + "...";
            return dumped;
        }
        // Nothing resolved at this pointer in either the buffer or the
        // skeleton. Show WHERE it points: "(unset)" alone cannot tell a row
        // missing from the buffer apart from a row pointing at a path that was
        // never built, and the two have different fixes.
        return "?? " + row.ptr.to_string();
    }

    // 10.3: `!config_data[row.ptr].get<bool>()` threw on an absent key, because
    // the non-const operator[] of a const json raises. An absent schema row
    // takes the table default first and then flips -- which is what makes a
    // dimmed row editable without hand-editing a file (8.4).
    // The one write path for a row, mirroring EffectiveValue's resolution. A
    // schema row writes through its pointer; a component row writes the param
    // of the entry with that name, appending the entry when it is missing.
    // Writing through `config_data[row.ptr]` instead would create a SECOND
    // entry at whatever index the pointer names -- the drift this whole change
    // exists to remove.
    void WriteRow(const MenuRow& row, json value) {
        if (row.component_path.empty()) {
            config_data[row.ptr] = std::move(value);
            return;
        }

        const std::string param = row.ptr.back();
        json& arr = config_data[mokuren::cfg::PointerFor(row.component_path)];
        if (!arr.is_array()) arr = json::array();
        for (json& e : arr) {
            if (!e.is_object()) continue;
            if (e.value("name", std::string()) != row.component_name) continue;
            e[param] = std::move(value);
            return;
        }

        json entry = json::object();
        entry["name"] = row.component_name;
        entry["enabled"] = true;
        entry[param] = std::move(value);
        arr.push_back(std::move(entry));
    }

    void ToggleRowBool(const MenuRow& row) {
        const json v = EffectiveValue(row);
        const bool current = v.is_boolean() ? v.get<bool>() : false;
        WriteRow(row, !current);
    }

    // The one place cursors are made valid. Render used to do it inline, which
    // let the draw path move state and two panels disagree mid-frame (8.4).
    void ClampCursors() {
        if (categories.empty()) {
            category_cursor = 0;
            subcat_cursor = 0;
            param_cursor = 0;
            return;
        }
        category_cursor = std::min(std::max(category_cursor, 0),
                                   static_cast<int>(categories.size()) - 1);

        auto& subs = subcategories[categories[category_cursor]];
        if (subs.empty()) {
            subcat_cursor = 0;
            param_cursor = 0;
            return;
        }
        subcat_cursor = std::min(std::max(subcat_cursor, 0),
                                 static_cast<int>(subs.size()) - 1);

        auto& rows =
            grouped_rows[categories[category_cursor]][subs[subcat_cursor]];
        if (rows.empty()) {
            param_cursor = 0;
            return;
        }
        param_cursor = std::min(std::max(param_cursor, 0),
                                static_cast<int>(rows.size()) - 1);
    }

    // `only_dirs` lists datasets: the directory layout (a folder holding
    // trades.csv / L2.csv) and .mkn recordings. Recordings were missing from
    // this list because only directories were collected, so a .mkn could never
    // be picked even though DataManager::LoadAll has accepted one since the mdf
    // reader landed -- from the UI, "not listed" and "rejected by the loader"
    // look identical.
    // Lists the entries a picker may write. NO sentinel is appended (10.7):
    // the old list pushed NO_DATASETS_FOUND as a selectable option, so a
    // missing folder could be written into a config as if it were a path. An
    // empty result means the picker refuses to write.
    std::vector<std::string> ScanDirectory(const std::string& folder, bool only_dirs = false) {
        std::vector<std::string> options;
        std::error_code ec;
        if (folder.empty() || !fs::exists(folder, ec)) return options;
        try {
            if (true) {
                for (const auto& entry : fs::recursive_directory_iterator(folder)) {
                    if (only_dirs) {
                        // a .mkn recording is a dataset in its own right: one
                        // file carries both prints and book, so it sits beside
                        // the csv directories rather than inside one.
                        const bool is_recording =
                            entry.is_regular_file() &&
                            entry.path().extension() == ".mkn";
                        if (entry.is_directory() || is_recording) {
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
        
        std::sort(options.begin(), options.end());
        return options;
    }

    void FlattenSubtree(const json& j, const std::string& path,
                        const std::string& category,
                        const std::string& sub = "General",
                        const std::string& comp_path = std::string(),
                        const std::string& comp_name = std::string()) {
        if (j.is_object()) {
            if (mokuren::cfg::IsGridNode(j)) {
                PushLeaf(path, category, sub, comp_path, comp_name);
                return;
            }
            for (auto& [key, val] : j.items()) {
                const std::string next_sub =
                    (sub == "General" && val.is_object()) ? key : sub;
                FlattenSubtree(val, path + "/" + key, category, next_sub,
                               comp_path, comp_name);
            }
        } else if (j.is_array()) {
            for (size_t i = 0; i < j.size(); ++i) {
                const std::string child = path + "/" + std::to_string(i);
                if (j[i].is_object() && j[i].contains("name") &&
                    j[i]["name"].is_string()) {
                    const std::string entry_name =
                        j[i]["name"].get<std::string>();
                    FlattenSubtree(j[i], child, category, entry_name, comp_path,
                                   entry_name);
                } else {
                    FlattenSubtree(j[i], child, category, sub, comp_path,
                                   comp_name);
                }
            }
        } else {
            PushLeaf(path, category, sub, comp_path, comp_name);
        }
    }

    void PushLeaf(const std::string& path, const std::string& category,
                  const std::string& sub, const std::string& comp_path,
                  const std::string& comp_name) {
        const std::string key = path.substr(path.find_last_of('/') + 1);
        // a component's `name` is its subcategory, not a row.
        if (key == "name" && sub != "General") return;

        // `path` is a json pointer ("/backtest/config_profile"); IsRetiredKey
        // compares dotted paths. Without this translation the two never agreed
        // and the retired row rendered -- the separator, not the list, was the
        // bug.
        std::string dotted = path;
        if (!dotted.empty() && dotted.front() == '/') dotted.erase(0, 1);
        std::replace(dotted.begin(), dotted.end(), '/', '.');
        if (IsRetiredKey(dotted)) return;

        MenuRow row;
        row.display_path = key;
        row.ptr = json::json_pointer(path);
        // the dotted array path and the entry's name are what a component row
        // is actually addressed by; `ptr` above is kept only for the diagnostic
        // string and for schema rows, where an index is not involved.
        row.component_path = comp_path;
        row.component_name = comp_name;
        grouped_rows[category][sub].push_back(std::move(row));
    }

    // The three sources of config.md 8.1, in one place. Everything the schema
    // does not name and these two lists do not cover is simply absent here --
    // which is the point: a typo like `risk.slipage_pct` must not become a row
    // that tunes nothing.
    void BuildMenu() {
        categories.clear();
        subcategories.clear();
        grouped_rows.clear();

        // (1) schema rows: one per table entry, filed under the table's own
        // category names. The invariant (section 2) made mechanical -- a field
        // added to the table appears here with its default, no file edited.
        for (const mokuren::cfg::Field& f : mokuren::cfg::Schema()) {
            MenuRow row;
            row.def = &f;
            row.display_path = f.label;
            row.ptr = SchemaPointer(f.path);
            if (f.kind == mokuren::cfg::Kind::DirPath ||
                f.kind == mokuren::cfg::Kind::FilePath) {
                row.is_selector = true;
                row.options = ScanDirectory(
                    f.selector_root == nullptr ? std::string()
                                               : std::string(f.selector_root),
                    f.kind == mokuren::cfg::Kind::DirPath);
                const json* cur = mokuren::cfg::Lookup(config_data, f.path);
                const std::string current =
                    (cur != nullptr && cur->is_string())
                        ? cur->get<std::string>()
                        : (f.default_value.is_string()
                               ? f.default_value.get<std::string>()
                               : std::string());
                row.current_idx = SeedSelection(row, current);
            }
            grouped_rows[f.category][f.subcategory].push_back(std::move(row));
        }

        // (2) component arrays. Always present in config_data -- the skeleton
        // supplies every catalogue entry, the file only overrides values -- so
        // the category shows on a profile that configures nothing.
        for (const std::string& path : ComponentArrayPaths()) {
            const json* node = mokuren::cfg::Lookup(config_data, path);
            if (node == nullptr || !node->is_array()) continue;
            FlattenSubtree(*node, std::string("/") + path,
                           CategoryForComponent(path), "General", path);
        }

        // (3) file-driven subtrees
        for (const char* path : kFileDrivenSubtrees) {
            const json* node = mokuren::cfg::Lookup(config_data, path);
            if (node == nullptr || !node->is_object()) continue;
            FlattenSubtree(*node, std::string("/") + path,
                           CategoryForComponent(path));
        }

        // (4) rows within a bucket: schema rows by table order, then the
        // discovered ones in array/file order.
        for (auto& [cat, subs] : grouped_rows) {
            for (auto& [sub, rows] : subs) {
                (void)sub;
                std::stable_sort(rows.begin(), rows.end(),
                                 [](const MenuRow& a, const MenuRow& b) {
                    if (a.def != nullptr && b.def != nullptr)
                        return a.def->order < b.def->order;
                    if (a.def != nullptr) return true;
                    if (b.def != nullptr) return false;
                    return false;
                });
            }
        }

        // (5) category then subcategory order
        for (auto& [cat, subs] : grouped_rows) {
            categories.push_back(cat);
            for (auto& [sub, rows] : subs) {
                (void)rows;
                subcategories[cat].push_back(sub);
            }
        }
        std::stable_sort(categories.begin(), categories.end(),
                         [](const std::string& a, const std::string& b) {
            const int oa = CategoryOrder(a), ob = CategoryOrder(b);
            if (oa != ob) return oa < ob;
            return a < b;
        });
        for (auto& [cat, subs] : subcategories) {
            std::stable_sort(subs.begin(), subs.end(),
                             [&cat](const std::string& a, const std::string& b) {
                if (a == "General") return b != "General";
                if (b == "General") return false;
                const int oa = SubcategoryOrder(cat, a);
                const int ob = SubcategoryOrder(cat, b);
                if (oa != ob) return oa < ob;
                return false;
            });
        }

        ClampCursors();
    }

    void OpenProfilePicker() {
        profile_options = ScanDirectory("configs", false);
        profile_cursor = 0;
        auto it = std::find(profile_options.begin(), profile_options.end(),
                            config_path);
        if (it != profile_options.end())
            profile_cursor =
                static_cast<int>(std::distance(profile_options.begin(), it));
        menu_state = MenuState::PROFILE_PICKER;
    }

    void HandleProfilePickerKey(uint32_t key) {
        if (key == NCKEY_ESC || key == 'p' || key == 'P') {
            menu_state = MenuState::PARAMETERS;
            return;
        }
        if (profile_options.empty()) return;
        if (key == NCKEY_UP && profile_cursor > 0) { --profile_cursor; return; }
        if (key == NCKEY_DOWN &&
            profile_cursor + 1 < static_cast<int>(profile_options.size())) {
            ++profile_cursor;
            return;
        }
        if (key == NCKEY_ENTER) {
            config_path = profile_options[profile_cursor];
            LoadConfigSmart(false);
        }
    }

    // Read-only. The old version wrote the file back when it was missing or
    // unparseable, and rewrote backtest.config_profile on every load (10.4),
    // so opening a profile changed it. Nothing is written here now: a broken
    // file shows up as a footer line and waits for a keystroke.
    void LoadConfigSmart(bool keep_cursors = false) {
        unknown_keys = 0;
        config_error.clear();

        // the file is a diff; the skeleton is the shape. Overlay merges the
        // two, component arrays by name, so the menu always sees every field
        // and every component the engine supports.
        config_data = skeleton_data;
        std::ifstream file(config_path);
        if (file.is_open()) {
            try {
                const json disk = json::parse(file);
                if (!disk.is_object()) {
                    throw std::runtime_error("root is not an object");
                }
                mokuren::cfg::Overlay(config_data, disk);
            } catch (const std::exception& e) {
                config_data = skeleton_data;
                config_error = std::string("unreadable: ") + e.what();
            }
        } else {
            // no file is a normal state: the skeleton IS the profile until the
            // first save writes the diff. It reports "no trigger armed" through
            // RefreshStatus, which is the only thing a new profile owes you.
            config_error.clear();
        }

        DropRetiredKeys(config_data);
        loaded_data = config_data;

        BuildMenu();
        RefreshStatus();

        if (!keep_cursors) {
            category_cursor = 0;
            subcat_cursor = 0;
            param_cursor = 0;
            menu_state = MenuState::CATEGORIES;
        }
        ClampCursors();
    }

    // What a run would say about this file, right now. The editor's most useful
    // output is "this profile does not load, and here is why" -- said before the
    // operator starts a scan and gets it as a mid-run abort instead.
    void RefreshStatus() {
        unknown_keys = mokuren::cfg::CountUnknownKeys(config_data);
        if (!config_error.empty()) return;  // a read error outranks the checker

        json flat = config_data;
        try {
            FlattenGrids(flat);
            (void)AppConfig::Load(flat, /*require_venue_credentials=*/false);
        } catch (const std::exception& e) {
            config_error = e.what();
        }
    }

    // The only write path. No re-read afterwards: the menu is rebuilt from the
    // in-memory json, so a toggle does not read back the file it just wrote
    // (section 11) and cannot race an external edit into the cursor state.
    void SaveAndReload() {
        {
            // only deviations reach the disk (component_schema.hpp Diff). A
            // key set back to its default leaves the file, so the profile stays
            // a diff and later table-default changes still apply to it.
            json sparse = mokuren::cfg::Diff(config_data, skeleton_data);
            if (sparse.is_null()) sparse = json::object();
            std::ofstream file(config_path);
            if (file.is_open()) {
                file << sparse.dump(2);
                loaded_data = config_data;
                config_error.clear();
            } else {
                config_error = "cannot write " + config_path;
            }
        }
        BuildMenu();
        RefreshStatus();
        ClampCursors();
    }

public:
    std::string GetActiveConfigPath() const { return config_path; }

    // restore a previously selected config profile. must be called before
    // OnEnter()/Render() so the flattened menu reflects the right file.
    void SetActiveConfigPath(const std::string& path) {
        if (path.empty() || path == config_path) return;
        // reloading when the path MATCHES would throw away the profile the
        // picker just selected: UIManager re-supplies the path it captured on
        // the way out, which is only equal to config_path when nothing
        // changed. Every edit is saved as it is made, so in-memory already
        // equals on-disk here.
        config_path = path;
        LoadConfigSmart(false);
    }

    PageConfig() { 
        fs::create_directories("configs"); 
        fs::create_directories("data"); // Stellt sicher, dass das HFT-Data-Dir existiert
        LoadConfigSmart(); 
    }
    
    bool BlocksGlobalHotkeys() const override {
        return menu_state == MenuState::INLINE_EDIT ||
               menu_state == MenuState::SUBMENU_EDIT ||
               menu_state == MenuState::PROFILE_PICKER;
    }

    // What the global header shows for this page (config.md 16.2). State, not
    // chrome: the profile in play, whether the buffer differs from the file,
    // and how many keys the loader would reject. Advisory only -- the footer
    // keeps the authoritative error text, because that one can be long.
    std::string StatusLine() const override {
        std::string s = config_path;
        if (config_data != loaded_data) s += "  [modified] *";
        if (unknown_keys > 0) {
            s += "  (" + std::to_string(unknown_keys) +
                 (unknown_keys == 1 ? " unknown key)" : " unknown keys)");
        }
        if (!config_error.empty()) s += "  [!] not loadable";
        return s;
    }

    void OnEnter() override { 
        // config is already loaded by the constructor or SetActiveConfigPath.
        // reloading here would reset cursors and re-read from disk unnecessarily.
    }

    // A modal list over `configs/*.json`. The row that used to do this was
    // backtest.config_profile; D6 deleted the key, so the picker gets a binding
    // and a header line of its own (17.3).
    void DrawProfilePicker(struct ncplane* p, unsigned int dimy, unsigned int dimx) {
        if (dimy < 12) return;

        const int w = std::min<int>(static_cast<int>(dimx) - 6, 90);
        const int h = std::min<int>(static_cast<int>(dimy) - 6,
                                    static_cast<int>(profile_options.size()) + 5);
        const int x0 = (static_cast<int>(dimx) - w) / 2;
        const int y0 = (static_cast<int>(dimy) - h) / 2;

        UITheme::StyleBackground(p);
        for (int i = 0; i < h; ++i)
            ncplane_putstr_yx(p, y0 + i, x0, std::string(w, ' ').c_str());

        UITheme::StyleDataValue(p);
        ncplane_putstr_yx(p, y0, x0, std::string(w, '=').c_str());
        ncplane_putstr_yx(p, y0 + h - 1, x0, std::string(w, '=').c_str());

        UITheme::StyleTextDefault(p);
        ncplane_putstr_yx(p, y0 + 1, x0 + 2, "[ PROFILE ]");

        UITheme::StyleTextMuted(p);
        ncplane_putstr_yx(p, y0 + h - 2, x0 + 2,
                          "[UP/DN] PICK   [ENTER] OPEN   [ESC] CANCEL");

        if (profile_options.empty()) {
            UITheme::StyleAlert(p);
            ncplane_putstr_yx(p, y0 + 3, x0 + 2,
                              "no profile found under configs/");
            return;
        }

        const int body = h - 5;
        const int first = std::max(
            0, std::min(profile_cursor - body / 2,
                        static_cast<int>(profile_options.size()) - body));
        for (int i = 0; i < body &&
                        first + i < static_cast<int>(profile_options.size());
             ++i) {
            const int idx = first + i;
            const bool active = (idx == profile_cursor);
            if (active) UITheme::StyleCursorActive(p);
            else UITheme::StyleTextMuted(p);
            ncplane_putstr_yx(p, y0 + 3 + i, x0 + 2, active ? ">" : " ");

            if (active || profile_options[idx] == config_path)
                UITheme::StyleDataValue(p);
            else
                UITheme::StyleTextDefault(p);
            ncplane_putstr_yx(p, y0 + 3 + i, x0 + 4,
                              profile_options[idx].c_str());
        }
    }

    void Render(struct ncplane* stdplane) override {
        unsigned int dimy, dimx;
        ncplane_dim_yx(stdplane, &dimy, &dimx);

        if (menu_state == MenuState::PROFILE_PICKER) {
            DrawProfilePicker(stdplane, dimy, dimx);
            return;
        }

        int start_y = 11; 

        if (categories.empty()) {
            UITheme::StyleAlert(stdplane);
            ncplane_putstr_yx(stdplane, start_y, 3, "ERR: NO EDITABLE PARAMS.");
            return;
        }

        int visible_rows = std::max(5, (int)dimy - start_y - 4);

        // no cursor clamping here (8.4): Render is a function of state, and a
        // draw that silently moves the cursor is how two panels end up
        // disagreeing mid-frame. ClampCursors() runs on every state change.
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
                        UITheme::StyleDataValue(stdplane);
                        if (row.is_selector) {
                            val_str = SelectedOption(row);
                        } else {
                            val_str = "[" + edit_buffer + "_]";
                            // cyan on the edit background, not red-on-red
                            // (10.11): an editable buffer must read as text.
                            UITheme::StyleEditBackground(stdplane);
                        }
                    } else {
                        val_str = FormatValue(row);
                        // a value the file does not carry is a table default.
                        // Dimming it is D4 made visible (16.2): at a glance,
                        // what the profile says versus what it inherits.
                        if (IsAbsent(row)) UITheme::StyleTextMuted(stdplane);
                        else if (is_active_param) UITheme::StyleDataValue(stdplane);
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

        // an invalid profile replaces the key hints with the reason (16.2).
        // A count and a message, not a review screen -- section 14 rejected
        // the screen.
        if (!config_error.empty()) {
            UITheme::StyleAlert(stdplane);
            std::string msg = config_error;
            const int room = static_cast<int>(dimx) - 6;
            if (room > 4 && static_cast<int>(msg.size()) > room) {
                msg = msg.substr(0, room - 3) + "...";
            }
            ncplane_putstr_yx(stdplane, dimy - 2, 3, msg.c_str());
        } else {
            // only the keys live here now. The profile path, [modified] and the
            // unknown-key count moved to the global header's status row (16.2),
            // so they stay visible on the scanner and the trade log too.
            UITheme::StyleTextMuted(stdplane);
            ncplane_putstr_yx(stdplane, dimy - 2, 3,
                              "[ENTER] EDIT   [</>] STEP   [P] PROFILE   [ESC] BACK");
        }
    }

    void HandleInput(uint32_t key) override {
        if (menu_state == MenuState::PROFILE_PICKER) {
            HandleProfilePickerKey(key);
            return;
        }

        // P opens the profile picker (17.3). It is not a menu row any more: D6
        // deleted backtest.config_profile, so "which file am I editing" is UI
        // state and needs a home of its own.
        if ((key == 'p' || key == 'P') && menu_state != MenuState::INLINE_EDIT &&
            menu_state != MenuState::SUBMENU_EDIT) {
            OpenProfilePicker();
            return;
        }

        if (categories.empty()) return;

        ClampCursors();
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
                if (RowIsBool(row)) {
                    ToggleRowBool(row);
                    SaveAndReload();
                } else if (RowIsNumeric(row)) {
                    menu_state = MenuState::SUBMENU_NAV;
                    sub_state.cursor_y = 0;
                    const json v = EffectiveValue(row);
                    if (RowIsGrid(row)) {
                        sub_state.is_range = true;
                        sub_state.min_str = DoubleToStr(v.value("min", 0.0));
                        sub_state.max_str = DoubleToStr(v.value("max", 0.0));
                        sub_state.step_str = DoubleToStr(v.value("step", 0.0));
                        sub_state.val_str = DoubleToStr(v.value("val", 0.0));
                    } else {
                        sub_state.is_range = false;
                        sub_state.val_str =
                            v.is_number() ? DoubleToStr(v.get<double>()) : "0";
                        sub_state.min_str = sub_state.val_str;
                        sub_state.max_str = sub_state.val_str;
                        sub_state.step_str = "1.0";
                    }
                } else {
                    menu_state = MenuState::INLINE_EDIT;
                    if (!row.is_selector) {
                        const json v = EffectiveValue(row);
                        edit_buffer = v.is_string() ? v.get<std::string>()
                                                    : std::string();
                    }
                }
            }
        }
        else if (menu_state == MenuState::INLINE_EDIT) {
            auto& row = rows[param_cursor];
            if (row.is_selector) {
                // nothing to pick means nothing to write: the sentinel option
                // is gone (10.7), so an empty list is an empty list.
                if (row.options.empty()) return;
                if (key == NCKEY_LEFT && row.current_idx > 0) row.current_idx--;
                else if (key == NCKEY_RIGHT &&
                         row.current_idx + 1 <
                             static_cast<int>(row.options.size())) {
                    row.current_idx++;
                } else if (key == NCKEY_ENTER) {
                    WriteRow(row, row.options[row.current_idx]);
                    SaveAndReload();
                    menu_state = MenuState::PARAMETERS;
                }
            } else {
                if (key == NCKEY_ENTER) {
                    WriteRow(row, edit_buffer);
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
                        // no "mode" key: it was the old shape detector's
                        // marker and IsGridNode wants min/max/step/val. Writing
                        // it kept a dead key alive in every grid saved here.
                        const double lo = ParseDouble(sub_state.min_str);
                        const double hi = ParseDouble(sub_state.max_str);
                        // keep the value the grid already resolved to, clamped
                        // into the new bounds. Resetting it to `min` threw away
                        // the point the sweep was anchored on, so every trip
                        // through this dialog silently moved it.
                        const json prev = EffectiveValue(row);
                        double keep = (prev.is_object() && prev.contains("val") &&
                                       prev["val"].is_number())
                                          ? prev["val"].get<double>()
                                          : lo;
                        if (keep < lo) keep = lo;
                        if (keep > hi) keep = hi;
                        json r_obj;
                        r_obj["min"] = lo;
                        r_obj["max"] = hi;
                        r_obj["step"] = ParseDouble(sub_state.step_str);
                        r_obj["val"] = keep;
                        WriteRow(row, r_obj);
                    } else {
                        WriteRow(row, ParseDouble(sub_state.val_str));
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

        ClampCursors();
    }
};
