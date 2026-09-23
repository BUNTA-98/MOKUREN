#pragma once

#include "aggregator.hpp"
#include "papertrader.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <vector>
#include <mutex>
#include <ctime>
#include <algorithm>
#include <string>



class UIManager {
private:
  const std::vector<Bar> &history_;
  const PaperTrader &trader_;
  std::mutex &data_mtx;

  bool is_input_mode_ = false;
  std::string jump_input_ = "";

  int footprint_scroll_ = 0;


  // Zeiger auf die aktuell ausgewählte Kerze für die Historien-Navigation
  size_t selected_bar_index_ = 0;

  // FTXUI Kern-Komponenten
  ftxui::ScreenInteractive screen_ = ftxui::ScreenInteractive::Fullscreen();

  // Hilfsmethoden zum Rendern von einzelnen Panels
  ftxui::Element RenderHeader();
  ftxui::Element RenderCandleDetails();
  ftxui::Element RenderTraderStats();
  ftxui::Element RenderFootprint();

public:
  UIManager(const std::vector<Bar> &history, const PaperTrader &trader, std::mutex &mtx);

  // Startet die interaktive TUI-Schleife
  void Run();
};
