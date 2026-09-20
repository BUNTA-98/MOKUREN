#include "csv_parser.hpp"
#include <fstream>
#include <iostream>
#include <sstream>

size_t
CSVLoader::ProcessBinanceCSV(const std::string &filepath,
                             std::function<void(const TradeEvent &)> on_trade) {
  std::ifstream file(filepath);
  if (!file.is_open()) {
    std::cerr << "Fehler: CSV-Datei konnte nicht geöffnet werden: " << filepath
              << std::endl;
    return 0;
  }

  std::string line;
  std::getline(file, line); // Header überspringen

  size_t processed_ticks = 0;

  while (std::getline(file, line)) {
    if (line.empty())
      continue;

    std::stringstream ss(line);
    std::string col[7];
    std::string token;
    int idx = 0;

    while (std::getline(ss, token, ',') && idx < 7) {
      col[idx++] = token;
    }
    if (idx < 7)
      continue;

    TradeEvent trade;
    trade.price = std::stod(col[1]);
    trade.quantity = std::stod(col[2]);
    trade.timestamp = std::stoll(col[5]);
    trade.is_buyer_maker =
        (col[6] == "true" || col[6] == "True" || col[6] == "1");

    // Trade ist fertig gelesen -> Sofort ans Callback (die main) übergeben
    on_trade(trade);

    processed_ticks++;
  }

  return processed_ticks;
}
