#pragma once
#include "market_context.hpp" // Nur für das TradeEvent struct benötigt
#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <vector>

// Resolved column indices for one CSV. `has_header == false` means the first
// line is already data, and the indices come from a known headerless layout
// (Binance Vision aggTrades/trades, Bybit public trades or Hyperliquid fills
// for trades; Binance Vision bookTicker for L2).
struct CsvColumns {
  bool has_header = false;
  // trades
  int ts = -1;
  int price = -1;
  int qty = -1;
  int maker = -1;
  // aggressor/taker side ("Buy"/"Sell", "B"/"A", ...). used when the feed
  // publishes the taker side instead of Binance's is_buyer_maker bit.
  int side = -1;
  // l2: one entry per depth level, in file order
  std::vector<int> bid_px;
  std::vector<int> bid_qty;
  std::vector<int> ask_px;
  std::vector<int> ask_qty;
};

// shared by csv_parser and DataManager so every reader agrees on what a header
// is and on the headerless fallback.
namespace csv_detail {

inline std::vector<std::string> SplitLine(const std::string &line) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t comma = line.find(',', start);
    std::string token = (comma == std::string::npos)
                            ? line.substr(start)
                            : line.substr(start, comma - start);
    token.erase(std::remove(token.begin(), token.end(), '\r'), token.end());
    out.push_back(std::move(token));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

inline std::string StripBom(std::string s) {
  if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
      static_cast<unsigned char>(s[1]) == 0xBB &&
      static_cast<unsigned char>(s[2]) == 0xBF) {
    s.erase(0, 3);
  }
  return s;
}

inline std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

// A header field is a label; a data field starts with a number (or a sign).
inline bool LooksNumeric(const std::string &raw) {
  const std::string s = StripBom(raw);
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '"')) ++i;
  if (i == s.size()) return false;
  if (s[i] == '-' || s[i] == '+') ++i;
  bool digit = false;
  for (; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (std::isdigit(c)) {
      digit = true;
      continue;
    }
    break;
  }
  return digit;
}

} // namespace csv_detail

class CSVLoader {
public:
  // Nimmt den Dateipfad und das Callback (die auszuführende Aktion pro Trade)
  static size_t
  ProcessBinanceCSV(const std::string &filepath,
                    std::function<void(const TradeEvent &)> on_trade);

  // Sniffs the first line of a trades CSV. Labelled headers from Binance,
  // Bybit and Hyperliquid are resolved by name. Headerless input is recognised
  // as Binance Vision trades/aggTrades, Bybit public trades or Hyperliquid
  // fills, in that order.
  static CsvColumns SniffTradeColumns(const std::string &first_line);

  // Sniffs the first line of an L2 CSV. Headerless input is read as Binance
  // Vision bookTicker:
  // update_id,bid_px,bid_qty,ask_px,ask_qty,transact_time,event_time.
  static CsvColumns SniffL2Columns(const std::string &first_line);
};
