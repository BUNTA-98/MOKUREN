#pragma once
#include <notcurses/notcurses.h>
#include <string>

// Abstrakte Basisklasse für alle UI-Seiten
class UIPage {
public:
    virtual ~UIPage() = default;
    
    // Wird aufgerufen, wenn die Seite vom Manager gezeichnet werden soll
    virtual void Render(struct ncplane* stdplane) = 0;
    
    // Wird aufgerufen, wenn eine Taste gedrückt wurde (die nicht global ist)
    virtual void HandleInput(uint32_t key) = 0;
    
    // Optional: Wird aufgerufen, wenn auf diese Seite gewechselt wird
    virtual void OnEnter() {}

  // NEU: Teilt dem Manager mit, ob globale Hotkeys blockiert werden sollen
    virtual bool BlocksGlobalHotkeys() const { return false; }

    // One line of page-owned state for the global header (config.md 16.2).
    // The manager draws it, the page decides what is worth saying, and the
    // default is empty so a page with nothing to report adds no clutter.
    virtual std::string StatusLine() const { return std::string(); }
};
