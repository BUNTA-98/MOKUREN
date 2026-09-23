/* TODO
scrolling candle
show candle start timestamp in details
volume profile
trade history/log an seite (BUY/SELL @ , closed @, profit)
go to candle by index (press (I) then type)

*/

#include "ui.hpp"

using namespace ftxui;

UIManager::UIManager(const std::vector<Bar> &history, const PaperTrader &trader,
                     std::mutex &mtx)
    : history_(history), trader_(trader), data_mtx(mtx) {
  if (!history_.empty()) {
    selected_bar_index_ = history_.size() - 1; // Standard: Neueste Kerze
  }
}

Element UIManager::RenderHeader() {
  return hbox({
             text(" MOKUREN V0.1 ") | bold | color(Color::Red),
             filler(),
             text(" (<-) flip (->)  | (q)uit ") | dim,
         }) |
         color(Color::White) | border | color(Color::Red);
}

Element UIManager::RenderCandleDetails() {

  Bar bar;
  {
    std::lock_guard<std::mutex> lock(data_mtx);

    if (history_.empty()) {
      return text("no candle in history.") | center;
    }

    bar = history_[selected_bar_index_];
  }

  // Unix-Timestamp in HH:MM:SS umwandeln
  std::time_t time_val = bar.timestamp_start / 1000;
  std::tm *time_info = std::localtime(&time_val);
  char time_str[32];
  std::strftime(time_str, sizeof(time_str), "%H:%M:%S", time_info);

  Color candle_color = (bar.close >= bar.open) ? Color::Green : Color::Red;

  return vbox({
             text("DETAILS ( " + std::to_string(selected_bar_index_) + " / " +
                  std::to_string(history_.size() - 1) + ")") |
                 bold,
             separator(),
             hbox({text(" Time:  "), text(time_str)}),
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

  double net_profit = trader_.GetNetProfit();
  int total_trades = trader_.GetTotalTrades();

  double winrate = 0.0;
  if (total_trades > 0) {
    winrate =
        (static_cast<double>(trader_.GetTradesWon()) / total_trades) * 100.0;
  }

  Color pnl_color = (net_profit >= 0) ? Color::Green : Color::Red;

  return vbox({text("TRADER STATS") | bold, separator(),
               hbox({text(" TOTAL        : "),
                     text(std::to_string(total_trades))}),
               hbox({text(" WINS         : "),
                     text(std::to_string(trader_.GetTradesWon())) |
                         color(Color::Green)}),
               hbox({text(" LOSSES       : "),
                     text(std::to_string(trader_.GetTradesLost())) |
                         color(Color::Red)}),
               hbox({text(" WINRATE      : "),
                     text(std::to_string(winrate) + " %")}),
               separator(),
               hbox({text(" TOTAL PNL    : "),
                     text("$ " + std::to_string(net_profit)) |
                         color(pnl_color)}),
               hbox({text(" FEES         : "),
                     text("$ " + std::to_string(trader_.GetTotalFeesPaid())) |
                         color(Color::GrayDark)})}) |
         border;
}

Element UIManager::RenderFootprint() {
  Bar bar; // local copy
  {
    std::lock_guard<std::mutex> lock(data_mtx); // get key

    if (history_.empty()) {
      return text("empty history") | border | flex;
    }

    bar = history_[selected_bar_index_];

  } // free key

  // Maximales Volumen der Kerze für die Skalierung des Profiles finden
  double max_vol = 0.0;
  for (int i = 0; i < MAX_GRID_LEVELS; ++i) {
    max_vol = std::max(max_vol,
                       bar.vap_grid[i].bid_volume + bar.vap_grid[i].ask_volume);
  }

  Elements rows;

  // 1. Überschrift
  rows.push_back(
      hbox({text(" BID ") | color(Color::RedLight) | bold |
                size(WIDTH, EQUAL, 8),
            text(" PRICE ") | bold | size(WIDTH, EQUAL, 10),
            text(" ASK ") | color(Color::GreenLight) | bold |
                size(WIDTH, EQUAL, 8),
            text(" | "), text("PROFILE") | bold | size(WIDTH, EQUAL, 12)}) |
      center);

  rows.push_back(separator());

  // format nachkomma
  auto fmt = [](double v) {
    if (v == 0.0)
      return std::string("0");
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f",
             v); // Schneidet sauber auf 2 Nachkommastellen ab
    return std::string(buf);
  };
  // 2. Das vap_grid rückwärts durchlaufen (höchster Preis zuerst)
  // MAX_GRID_LEVELS muss hier verfügbar sein (evtl. aggregator.hpp inkludieren)
  for (int i = MAX_GRID_LEVELS - 1; i >= 0; --i) {

    // Überspringe ungenutzte Level (in deinem Aggregator ist price dann 0.0)
    if (bar.vap_grid[i].price == 0.0)
      continue;

    double price = bar.vap_grid[i].price;
    double bid_v = bar.vap_grid[i].bid_volume;
    double ask_v = bar.vap_grid[i].ask_volume;
    double total_v = bid_v + ask_v;

    // Volume Profile Balken generieren (max. 10 Zeichen lang)
    int bar_length =
        (max_vol > 0.0) ? static_cast<int>((total_v / max_vol) * 10.0) : 0;
    std::string profile_str = "";
    for (int b = 0; b < bar_length; ++b)
      profile_str += "█";

    // regular volume style
    auto bid_style = color(Color::Red);
    auto ask_style = color(Color::Green);

    // imbalance colors
    double imbalance_ratio = 3.0;
    double min_vol = 0.01;

    // bull imbalance
    if (i > 0 && ask_v >= min_vol) {
      double bid_below = bar.vap_grid[i - 1].bid_volume;
      if (ask_v >= bid_below * imbalance_ratio) {
        ask_style = bgcolor(Color::Green) | color(Color::White) | bold;
      }
    }

    // bear imbalance
    if (i < MAX_GRID_LEVELS - 1 && bid_v >= min_vol) {
      double ask_above = bar.vap_grid[i + 1].ask_volume;
      if (bid_v >= ask_above * imbalance_ratio) {
        // Hervorhebung: Weißer Text auf rotem Grund
        bid_style = bgcolor(Color::Red) | color(Color::White) | bold;
      }
    }

    // 3. Zeile mit dynamischen Styles zusammenbauen
    auto row = hbox({text(fmt(bid_v)) | bid_style | align_right |
                         size(WIDTH, EQUAL, 8),
                     text(" | ") | dim,
                     text(fmt(price)) | size(WIDTH, EQUAL, 10) | center,
                     text(" | ") | dim,
                     text(fmt(ask_v)) | ask_style | size(WIDTH, EQUAL, 8),
                     text(" | ") | dim,
                     text(profile_str) | color(Color::BlueLight) |
                         size(WIDTH, EQUAL, 12)}) |
               center;

    // POC marker
    if (price == bar.poc_price && bar.poc_price > 0.0) {
      row = row | bgcolor(Color::GrayDark) | bold;
    }

    rows.push_back(row);
  }

  // Wenn die Kerze keine Volumendaten hat (z.B. frisch geöffnet)
  if (rows.size() == 2) {
    rows.push_back(text("Noch keine Ticks...") | dim | center);
  }

  return vbox(rows) | focusPosition(0, footprint_scroll_) | vscroll_indicator | yframe | border | flex;
}

void UIManager::Run() {

  auto renderer = Renderer([&] {
    // main layout
    auto main_layout = vbox({
        RenderHeader(),
        hbox({
            RenderCandleDetails() | size(WIDTH, LESS_THAN, 30),
            RenderFootprint() | flex,
            RenderTraderStats() | size(WIDTH, LESS_THAN, 30),
        }) | flex,
    });

    // index input overlay
    Element overlay = is_input_mode_
                          ? window(text(" Jump to Index (Enter/Esc) "),
                                   text(jump_input_ + "_") | bold) |
                                clear_under | bgcolor(Color::Black) |
                                color(Color::Red) | center
                          : text(""); // unsichtbar, wenn nicht aktiv

    // dbox legt das Overlay über das main_layout
    return dbox({main_layout, overlay});
  });

  auto component = CatchEvent(renderer, [&](Event event) {
    // input mode
    if (is_input_mode_) {
      if (event == Event::Return) { // on enter press mutex gets locked
        if (!jump_input_.empty()) {
          int idx = std::stoi(jump_input_);
          std::lock_guard<std::mutex> lock(data_mtx);
          selected_bar_index_ =
              std::max(0, std::min((int)history_.size() - 1, idx));
        } // mutex unlocked
        is_input_mode_ = false;
        return true;
      }
      if (event == Event::Escape) {
        is_input_mode_ = false;
        return true;
      }
      if (event == Event::Backspace && !jump_input_.empty()) {
        jump_input_.pop_back();
        return true;
      }
      // Nur Zahlen zulassen
      if (event.is_character() && std::isdigit(event.character()[0])) {
        jump_input_ += event.character();
        return true;
      }
      return true; // Blockiert andere Events, während wir tippen
    }

    if (event == Event::Character('i') || event == Event::Character('I')) {
      is_input_mode_ = true;
      jump_input_ = "";
      return true;
    }

// --- NEU: Trackpad / Maus-Rad ---
    if (event.is_mouse()) {
        if (event.mouse().button == Mouse::WheelDown) {
            footprint_scroll_++;
            return true; // true = UI neu zeichnen
        }
        if (event.mouse().button == Mouse::WheelUp) {
            if (footprint_scroll_ > 0) footprint_scroll_--;
            return true;
        }
    }

// --- NEU: Pfeiltasten zum Scrollen ---
    if (event == Event::ArrowUp) {
        if (footprint_scroll_ > 0) footprint_scroll_--;
        return true;
    }
    if (event == Event::ArrowDown) {
        footprint_scroll_++;
        return true;
    }


    if (event == Event::ArrowLeft) {
      if (selected_bar_index_ > 0)
        selected_bar_index_--;
      footprint_scroll_ = 0;
      return true;
    }

    if (event == Event::ArrowRight) {
      std::lock_guard<std::mutex> lock(data_mtx);
      if (selected_bar_index_ + 1 < history_.size())
        selected_bar_index_++;
      footprint_scroll_ = 0;
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
