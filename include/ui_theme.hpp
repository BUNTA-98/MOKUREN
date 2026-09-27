#pragma once
#include <notcurses/notcurses.h>

namespace UITheme {
    
    // ==========================================
    // 1. DIE PALETTE (Rohe Farben)
    // ==========================================
    inline void RawRed(struct ncplane* p)      { ncplane_set_fg_rgb8(p, 255, 10, 20); }
    inline void RawDarkRed(struct ncplane* p)  { ncplane_set_fg_rgb8(p, 200, 15, 25); }
    inline void RawCyan(struct ncplane* p)     { ncplane_set_fg_rgb8(p, 0, 200, 255); }
    inline void RawOrange(struct ncplane* p)   { ncplane_set_fg_rgb8(p, 255, 64, 0); }
    
    // Alte Farben (werden aktuell nicht genutzt, bleiben aber als Reserve)
    inline void RawYellow(struct ncplane* p)   { ncplane_set_fg_rgb8(p, 255, 204, 0); }
    inline void RawGray(struct ncplane* p)     { ncplane_set_fg_rgb8(p, 80, 80, 80); }
    inline void RawWhite(struct ncplane* p)    { ncplane_set_fg_rgb8(p, 240, 240, 240); }

    // ==========================================
    // 2. DIE SEMANTIK (Deine neue Pipeline)
    // ==========================================
    
    // Typografie & Struktur (Alles Rot)
    inline void StyleTextDefault(struct ncplane* p) { RawRed(p); }       
    inline void StyleTextMuted(struct ncplane* p)   { RawDarkRed(p); }   
    
    // UI-Elemente & Interaktion (Alles Aktive ist Cyan)
    inline void StyleCursorActive(struct ncplane* p) { RawCyan(p); }     
    inline void StyleDataValue(struct ncplane* p)    { RawCyan(p); }     
    inline void StyleAlert(struct ncplane* p)        { RawRed(p); }      
    
    // Hintergründe
    inline void StyleBackground(struct ncplane* p) { 
        ncplane_set_bg_default(p); 
    }
    
    inline void StyleEditBackground(struct ncplane* p) { 
        ncplane_set_bg_rgb8(p, 150, 10, 30); 
        ncplane_set_bg_alpha(p, NCALPHA_BLEND); 
    }

    // ==========================================
    // 3. FOOTPRINT SPEZIFISCH
    // ==========================================
    inline void StylePOC(struct ncplane* p) { 
        RawOrange(p); 
        ncplane_set_bg_default(p); 
    }
    
    inline void StyleBid(struct ncplane* p) { 
        RawRed(p); 
        ncplane_set_bg_default(p); 
    }
    
    inline void StyleAsk(struct ncplane* p) { 
        RawCyan(p); 
        ncplane_set_bg_default(p); 
    }
    
    inline void StyleBidImbalance(struct ncplane* p) { 
        ncplane_set_fg_rgb8(p, 0, 0, 0);       // Schwarzer Text
        ncplane_set_bg_rgb8(p, 255, 10, 20);   // Roter Hintergrund
    }
    
    inline void StyleAskImbalance(struct ncplane* p) { 
        ncplane_set_fg_rgb8(p, 0, 0, 0);       // Schwarzer Text
        ncplane_set_bg_rgb8(p, 0, 200, 255);   // Blauer/Cyan Hintergrund
    }
    
    inline void StyleVolumeBar(struct ncplane* p) { 
        ncplane_set_fg_rgb8(p, 100, 100, 100); // Gedimmtes Grau für die Balken
        ncplane_set_bg_default(p); 
    }
}