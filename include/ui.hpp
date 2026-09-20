#pragma once

#include "aggregator.hpp"
#include "papertrader.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <vector>

class UIManager {
private:
  const std::vector<Bar> &history_;
  const PaperTrader &trader_;

  // Zeiger auf die aktuell ausgewählte Kerze für die Historien-Navigation
  size_t selected_bar_index_ = 0;

  // FTXUI Kern-Komponenten
  ftxui::ScreenInteractive screen_ = ftxui::ScreenInteractive::Fullscreen();

  // Hilfsmethoden zum Rendern von einzelnen Panels
  ftxui::Element RenderHeader();
  ftxui::Element RenderCandleDetails();
  ftxui::Element RenderTraderStats();

public:
  UIManager(const std::vector<Bar> &history, const PaperTrader &trader);

  // Startet die interaktive TUI-Schleife
  void Run();
};