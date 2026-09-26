#pragma once
#include "ui_pages.hpp"
#include "ui_theme.hpp"

class UIInspector : public UIPage {
public:
    void Render(struct ncplane* stdplane) override {
        UITheme::StyleTextDefault(stdplane);
        ncplane_putstr_yx(stdplane, 10, 3, "[ ORDERFLOW RENDER ZONE ]");
        
        UITheme::StyleTextMuted(stdplane);
        ncplane_putstr_yx(stdplane, 12, 3, "Awaiting Footprint Data Stream...");
    }

    void HandleInput(uint32_t key) override {
        // Hier kommt später die Logik rein, um mit den Pfeiltasten
        // durch die Footprint-Kerzen zu scrollen.
    }
};
