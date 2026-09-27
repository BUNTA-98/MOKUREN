#pragma once
#include <notcurses/notcurses.h>

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
};
