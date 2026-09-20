#pragma once
#include "aggregator.hpp" // Nutzt deine exakte Bar-Struktur!
#include <vector>

struct MarketContext {
  const std::vector<Bar> &history; // Abgeschlossene Kerzen
  const Bar &live_bar;             // Deine gerade aktive Kerze
};
