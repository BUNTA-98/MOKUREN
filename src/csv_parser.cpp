#include "csv_parser.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include <algorithm>

size_t CSVLoader::ProcessBinanceCSV(const std::string &filepath,
                                    std::function<void(const TradeEvent &)> on_trade) {
  std::ifstream file(filepath);
  if (!file.is_open()) {
    std::cerr << "Fehler: CSV-Datei konnte nicht geöffnet werden: " << filepath << std::endl;
    return 0;
  }

  std::string line;
  if (!std::getline(file, line)) return 0; // Header auslesen

  // 1. HEADER DYNAMISCH PARSEN
  std::stringstream header_ss(line);
  std::string col_name;
  std::vector<std::string> headers;
  
  while (std::getline(header_ss, col_name, ',')) {
      // Versteckte Wagenrückläufe (Windows-Formate) entfernen
      col_name.erase(std::remove(col_name.begin(), col_name.end(), '\r'), col_name.end());
      headers.push_back(col_name);
  }

  int idx_price = -1, idx_qty = -1, idx_time = -1, idx_maker = -1;
  
  for (size_t i = 0; i < headers.size(); ++i) {
      if (headers[i] == "price" || headers[i] == "p") idx_price = i;
      else if (headers[i] == "qty" || headers[i] == "quantity" || headers[i] == "q") idx_qty = i;
      else if (headers[i] == "time" || headers[i] == "transact_time" || headers[i] == "T") idx_time = i;
      else if (headers[i] == "is_buyer_maker" || headers[i] == "m") idx_maker = i;
  }

  // Fallback: Falls die CSV gar keinen Header hat, nehmen wir das aggTrades-Standard-Layout
  if (idx_price == -1 || idx_qty == -1 || idx_time == -1 || idx_maker == -1) {
      idx_price = 1;
      idx_qty = 2;
      idx_time = 5;
      idx_maker = 6;
  }

  int max_idx = std::max({idx_price, idx_qty, idx_time, idx_maker});
  size_t processed_ticks = 0;

  // 2. HIGHSPEED DATA LOOP
  while (std::getline(file, line)) {
    if (line.empty()) continue;

    std::stringstream ss(line);
    std::string col[15]; // Statisch für maximalen Speed, 15 Spalten reichen immer
    std::string token;
    int idx = 0;

    while (std::getline(ss, token, ',') && idx < 15) {
      col[idx++] = token;
    }
    
    // Wenn die Zeile unvollständig ist, überspringen
    if (idx <= max_idx) continue;

    try {
        TradeEvent trade;
        trade.price = std::stod(col[idx_price]);
        trade.quantity = std::stod(col[idx_qty]);
        
        // HIER WAR DER FEHLER: Kein "/ 1000" mehr! Binance-Daten sind bereits in Millisekunden.
        trade.timestamp = std::stoll(col[idx_time]); 

        const std::string& m_str = col[idx_maker];
        trade.is_buyer_maker = (m_str == "true" || m_str == "True" || m_str == "1");

        on_trade(trade);
        processed_ticks++;
    } catch (...) {
        // Falls eine kaputte Zeile dazwischenrutscht, crasht die Engine nicht, sondern ignoriert sie.
        continue;
    }
  }

  return processed_ticks;
}