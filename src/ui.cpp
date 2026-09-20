#include "ui.hpp"

using namespace ftxui;

UIManager::UIManager(const std::vector<Bar> &history, const PaperTrader &trader)
    : history_(history), trader_(trader) {
  if (!history_.empty()) {
    selected_bar_index_ = history_.size() - 1; // Standard: Neueste Kerze
  }
}

Element UIManager::RenderHeader() {
  return hbox({
             text(" [QUANT ENGINE TUI] ") | bold | color(Color::Yellow),
             filler(),
             text(" Pfeil Links/Rechts: Kerze wechseln | Q: Beenden ") | dim,
         }) |
         border;
}

Element UIManager::RenderCandleDetails() {
  if (history_.empty()) {
    return text("Keine Kerzen-Daten vorhanden.") | center;
  }

  const Bar &bar = history_[selected_bar_index_];

  // Farbliche Markierung für Bullish / Bearish
  Color candle_color = (bar.close >= bar.open) ? Color::Green : Color::Red;

  return vbox({
             text("KERZEN DETAILS (Index: " +
                  std::to_string(selected_bar_index_) + " / " +
                  std::to_string(history_.size() - 1) + ")") |
                 bold,
             separator(),
             hbox({text(" Open:  "),
                   text(std::to_string(bar.open)) | color(candle_color)}),
             hbox({text(" High:  "), text(std::to_string(bar.high))}),
             hbox({text(" Low:   "), text(std::to_string(bar.low))}),
             hbox({text(" Close: "),
                   text(std::to_string(bar.close)) | color(candle_color)}),
             hbox({text(" Vol:   "), text(std::to_string(bar.total_volume))}),
         }) |
         border;
}

Element UIManager::RenderTraderStats() {
  return vbox({
             text("TRADING STATUS") | bold,
             separator(),
             text("Status: Bereit für Auswertung") | color(Color::Cyan),
         }) |
         border;
}

void UIManager::Run() {
  // Haupt-Layout zusammenbauen
  auto renderer = Renderer([&] {
    return vbox({
        RenderHeader(),
        hbox({
            RenderCandleDetails() | flex,
            RenderTraderStats() | flex,
        }),
    });
  });

  // Tastatureingaben abfangen
  auto component = CatchEvent(renderer, [&](Event event) {
    if (event == Event::ArrowLeft) {
      if (selected_bar_index_ > 0) {
        selected_bar_index_--;
      }
      return true; // Event wurde verarbeitet
    }
    if (event == Event::ArrowRight) {
      if (selected_bar_index_ + 1 < history_.size()) {
        selected_bar_index_++;
      }
      return true;
    }
    if (event == Event::Character('q') || event == Event::Character('Q')) {
      screen_.ExitLoopClosure()();
      return true;
    }
    return false;
  });

  screen_.Loop(component);
}
