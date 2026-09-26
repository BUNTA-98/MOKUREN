#pragma once
#include <notcurses/notcurses.h>

namespace UITheme {
    
    // ==========================================
    // 1. DIE PALETTE (Rohe Farben)
    // ==========================================
    inline void RawRed(struct ncplane* p)      { ncplane_set_fg_rgb8(p, 255, 10, 20); }
    inline void RawDarkRed(struct ncplane* p)  { ncplane_set_fg_rgb8(p, 200, 15, 25); }
    inline void RawCyan(struct ncplane* p)     { ncplane_set_fg_rgb8(p, 0, 200, 255); }
    inline void RawOrange(struct ncplane* p)    { ncplane_set_fg_rgb8(p, 255, 64, 0); }
    // Alte Farben (werden aktuell nicht genutzt, bleiben aber als Reserve)
    inline void RawYellow(struct ncplane* p)   { ncplane_set_fg_rgb8(p, 255, 204, 0); }
    inline void RawGray(struct ncplane* p)     { ncplane_set_fg_rgb8(p, 80, 80, 80); }
    inline void RawWhite(struct ncplane* p)    { ncplane_set_fg_rgb8(p, 240, 240, 240); }

    // ==========================================
    // 2. DIE SEMANTIK (Deine neue Pipeline)
    // ==========================================
    
    // Typografie & Struktur (Alles Rot)
    inline void StyleTextDefault(struct ncplane* p) { RawRed(p); }       // Normaler Text leuchtet Rot
    inline void StyleTextMuted(struct ncplane* p)   { RawDarkRed(p); }   // Trennlinien und inaktives Zeug 
    
    // UI-Elemente & Interaktion (Alles Aktive ist Cyan)
    inline void StyleCursorActive(struct ncplane* p) { RawCyan(p); }     // Cursor und linke Navigation
    inline void StyleDataValue(struct ncplane* p)    { RawCyan(p); }     // Die Parameter-Werte rechts
    inline void StyleAlert(struct ncplane* p)        { RawRed(p); }      // Warnungen bleiben im normalen Rot
    
    // Hintergründe
    inline void StyleBackground(struct ncplane* p) { 
        ncplane_set_bg_default(p); 
    }
    
    inline void StyleEditBackground(struct ncplane* p) { 
        ncplane_set_bg_rgb8(p, 150, 10, 30); 
        ncplane_set_bg_alpha(p, NCALPHA_BLEND); 
    }
}
