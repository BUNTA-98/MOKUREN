#pragma once
#include <notcurses/notcurses.h>
#include <algorithm>

namespace UITheme {
    
    // ==========================================
    // 1. DIE PALETTE (Rohe Farben)
    // ==========================================
    inline void RawRed(struct ncplane* p)      { ncplane_set_fg_rgb8(p, 255, 10, 20); }
    inline void RawDarkRed(struct ncplane* p)  { ncplane_set_fg_rgb8(p, 200, 15, 25); }
    inline void RawCyan(struct ncplane* p)     { ncplane_set_fg_rgb8(p, 0, 200, 255); }
    inline void RawOrange(struct ncplane* p)   { ncplane_set_fg_rgb8(p, 255, 128, 0); } 
    
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
    inline void StyleCursorActive(struct ncplane* p) { RawOrange(p); }     
    inline void StyleDataValue(struct ncplane* p)    { RawCyan(p); }     
    inline void StyleAlert(struct ncplane* p)        { RawOrange(p); }      
    
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
        ncplane_set_fg_rgb8(p, 0, 0, 0);       
        ncplane_set_bg_rgb8(p, 255, 10, 20);   
    }
    
    inline void StyleAskImbalance(struct ncplane* p) { 
        ncplane_set_fg_rgb8(p, 0, 0, 0);       
        ncplane_set_bg_rgb8(p, 0, 200, 255);   
    }
    
    inline void StyleVolumeBar(struct ncplane* p) { 
        ncplane_set_fg_rgb8(p, 100, 100, 100); 
        ncplane_set_bg_default(p); 
    }

    // --- NEU: Chart-Kerzen (Blau statt Grün!) ---
    inline void StyleCandleUp(struct ncplane* p) { 
        RawCyan(p); 
        ncplane_set_bg_default(p); 
    }
    
    inline void StyleCandleDown(struct ncplane* p) { 
        RawRed(p); 
        ncplane_set_bg_default(p); 
    }
    // --------------------------------------------

    // ==========================================
    // 4. HEATMAP FARB-ENGINE
    // ==========================================
    inline void StyleHeatmapCell(struct ncplane* p, double val) { 
        if(val < 0.0) val = 0.0;
        if(val > 1.0) val = 1.0;
        
        int r, g, b;
        if (val < 0.5) {
            double t = val * 2.0; 
            r = 255;
            g = 10 + (int)(t * 118);
            b = 20 - (int)(t * 20);
        } else {
            double t = (val - 0.5) * 2.0;
            r = 255 - (int)(t * 255);
            g = 128 + (int)(t * 72);
            b = (int)(t * 255);
        }
        
        ncplane_set_fg_rgb8(p, r, g, b); 
        ncplane_set_bg_default(p); 
    }
}