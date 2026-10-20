#include "csv_parser.hpp"
#include "timestamp_utils.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cctype>

namespace {
// token helpers shared by the row parser and the header sniffer. they are
// defined with the sniffing helpers further down.
std::string TrimToken(const std::string &raw);
int ParseSideToken(const std::string &raw);
} // namespace

size_t CSVLoader::ProcessBinanceCSV(const std::string &filepath,
                                    std::function<void(const TradeEvent &)> on_trade) {
  std::ifstream file(filepath);
  if (!file.is_open()) {
    std::cerr << "Fehler: CSV-Datei konnte nicht geöffnet werden: " << filepath << std::endl;
    return 0;
  }

  std::string line;
  if (!std::getline(file, line)) return 0;

  // one sniffer for this parser and DataManager::LoadAllTrades: a file whose
  // first line is already data is no longer silently lost when that line is
  // eaten as a header.
  const CsvColumns cols = SniffTradeColumns(line);
  if (cols.ts < 0 || cols.price < 0 || cols.qty < 0 ||
      (cols.maker < 0 && cols.side < 0)) {
    std::cerr << "Fehler: unbekanntes CSV-Layout "
                 "(ts/price/qty und maker|side fehlen): "
              << filepath << std::endl;
    return 0;
  }
  const int max_idx =
      std::max({cols.ts, cols.price, cols.qty, cols.maker, cols.side});

  size_t processed_ticks = 0;

  auto handle_row = [&](const std::string &row) {
    if (row.empty()) return;

    std::stringstream ss(row);
    std::string col[32];
    int n = 0;
    while (n < 32 && std::getline(ss, col[n], ',')) ++n;
    if (n <= max_idx) return;

    // std::getline keeps the \r of CRLF files on the last token; stod/stoll
    // stop at it, but the maker comparison would not.
    if (!col[n - 1].empty() && col[n - 1].back() == '\r') col[n - 1].pop_back();

    try {
      TradeEvent trade{};
      trade.price = std::stod(col[cols.price]);
      trade.quantity = std::stod(col[cols.qty]);
      trade.timestamp = NormalizeEpochToMillis(std::stoll(col[cols.ts]));

      if (cols.maker >= 0) {
        const std::string m = csv_detail::Lower(TrimToken(col[cols.maker]));
        trade.is_buyer_maker = (m == "true" || m == "1");
      } else {
        // Bybit and Hyperliquid publish the taker side instead of the maker
        // bit: a taker sell means the resting bid was hit, so the buyer was
        // the maker. mirrors Binance's is_buyer_maker.
        trade.is_buyer_maker = (ParseSideToken(col[cols.side]) < 0);
      }

      on_trade(trade);
      processed_ticks++;
    } catch (...) {
      // a corrupt row must not abort the replay
    }
  };

  if (!cols.has_header) handle_row(line);
  while (std::getline(file, line)) handle_row(line);

  return processed_ticks;
}

namespace {

// header labels are normalised to lowercase alphanumerics only, so
// "Best_Bid_Price", "best bid price" and "bestBidPrice" all collapse to
// "bestbidprice". data rows never reach these helpers: a numeric token cannot
// match a label, which is what keeps the headerless fallback below safe.
std::string NormalizeLabel(const std::string &raw) {
  const std::string s = csv_detail::Lower(csv_detail::StripBom(raw));
  std::string out;
  out.reserve(s.size());
  for (const unsigned char c : s) {
    if (std::isalnum(c)) out.push_back(static_cast<char>(c));
  }
  return out;
}

bool IsTradeTimeLabel(const std::string &n) {
  return n == "t" || n == "ts" || n == "time" || n == "timestamp" ||
         n == "transacttime" || n == "transactiontime" ||
         n == "tradetime" || n == "eventtime";
}

bool IsTradePriceLabel(const std::string &n) {
  return n == "p" || n == "px" || n == "price" || n == "lastprice" ||
         n == "tradeprice" || n == "execprice";
}

bool IsTradeQtyLabel(const std::string &n) {
  // quote_qty / quote_volume are notional values, not the traded quantity.
  if (n.find("quote") != std::string::npos) return false;
  return n == "q" || n == "qty" || n == "quantity" || n == "size" ||
         n == "sz" || n == "amount" || n == "volume" || n == "vol";
}

bool IsTradeMakerLabel(const std::string &n) {
  return n == "m" || n == "maker" || n == "buyermaker" ||
         n == "isbuyermaker" || n == "isbuyer" || n == "aggressorisbuyer";
}

bool IsTradeSideLabel(const std::string &n) {
  return n == "side" || n == "aggressorside" || n == "takerside" ||
         n == "sideindicator";
}

// side first, then price vs. qty: "best_bid_qty" contains both "bid" and "qty",
// so it may only be classified once the side is known.
bool IsBidSideLabel(const std::string &n) {
  return n == "b" || n == "bp" || n == "bq" || n == "bs" ||
         n.find("bid") != std::string::npos ||
         n.find("buy") != std::string::npos;
}

bool IsAskSideLabel(const std::string &n) {
  return n == "a" || n == "ap" || n == "aq" || n == "as" ||
         n.find("ask") != std::string::npos ||
         n.find("sell") != std::string::npos;
}

bool IsPricePart(const std::string &n) {
  return n == "b" || n == "a" || n == "p" || n == "px" || n == "bp" ||
         n == "ap" || n.find("price") != std::string::npos ||
         n.find("px") != std::string::npos;
}

bool IsQtyPart(const std::string &n) {
  return n == "q" || n == "bq" || n == "aq" || n == "bs" || n == "as" ||
         n.find("qty") != std::string::npos ||
         n.find("quantity") != std::string::npos ||
         n.find("size") != std::string::npos ||
         n.find("amount") != std::string::npos ||
         n.find("vol") != std::string::npos;
}

std::string TrimToken(const std::string &raw) {
  std::string s = csv_detail::StripBom(raw);
  size_t b = 0;
  size_t e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '"')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' ||
                   s[e - 1] == '"' || s[e - 1] == '\r')) {
    --e;
  }
  return s.substr(b, e - b);
}

// 1 = taker bought (aggressive buy), -1 = taker sold, 0 = unrecognised.
// Bybit exports "Buy"/"Sell", Hyperliquid uses "B"/"A"; both name the
// aggressor, never the maker.
int ParseSideToken(const std::string &raw) {
  const std::string s = csv_detail::Lower(TrimToken(raw));
  if (s == "b" || s == "buy" || s == "bid" || s == "long" ||
      s == "1" || s == "true") {
    return 1;
  }
  if (s == "a" || s == "s" || s == "sell" || s == "ask" || s == "short" ||
      s == "-1" || s == "false") {
    return -1;
  }
  return 0;
}

// side sniffing without headers: only word forms, so a price of exactly 1
// cannot be mistaken for a side token.
bool LooksSideWord(const std::string &raw) {
  const std::string s = csv_detail::Lower(TrimToken(raw));
  return s == "b" || s == "a" || s == "buy" || s == "sell" ||
         s == "bid" || s == "ask" || s == "long" || s == "short";
}

// a timestamp is a number with at least ten digits (seconds, ms, us or ns).
bool LooksTimestamp(const std::string &raw) {
  if (!csv_detail::LooksNumeric(raw)) return false;
  const std::string s = csv_detail::StripBom(raw);
  size_t digits = 0;
  for (const char c : s) {
    if (std::isdigit(static_cast<unsigned char>(c))) ++digits;
  }
  return digits >= 10;
}

bool LooksBoolean(const std::string &raw) {
  const std::string s = csv_detail::Lower(TrimToken(raw));
  return s == "true" || s == "false" || s == "0" || s == "1";
}

// strict word form: used to tell a Binance trades file apart from a book. the
// 0/1 form would collide with an all-zero event_time in a bookTicker file.
bool LooksBoolWord(const std::string &raw) {
  const std::string s = csv_detail::Lower(TrimToken(raw));
  return s == "true" || s == "false";
}

} // namespace

CsvColumns CSVLoader::SniffTradeColumns(const std::string &first_line) {
  CsvColumns cols;
  if (first_line.empty()) return cols;

  const std::vector<std::string> fields = csv_detail::SplitLine(first_line);

  // 1) labelled header. partial matches are still reported as a header so the
  // caller fails with "unbekanntes CSV-Layout" instead of re-reading the label
  // row as data.
  bool any_label = false;
  for (size_t i = 0; i < fields.size(); ++i) {
    const std::string n = NormalizeLabel(fields[i]);
    if (n.empty()) continue;
    const int idx = static_cast<int>(i);

    if (cols.ts < 0 && IsTradeTimeLabel(n)) {
      cols.ts = idx;
      any_label = true;
    } else if (cols.price < 0 && IsTradePriceLabel(n)) {
      cols.price = idx;
      any_label = true;
    } else if (cols.qty < 0 && IsTradeQtyLabel(n)) {
      cols.qty = idx;
      any_label = true;
    } else if (cols.maker < 0 && IsTradeMakerLabel(n)) {
      cols.maker = idx;
      any_label = true;
    } else if (cols.side < 0 && IsTradeSideLabel(n)) {
      cols.side = idx;
      any_label = true;
    }
  }
  if (any_label) {
    cols.has_header = true;
    return cols;
  }

  const size_t n = fields.size();

  // 2) headerless Binance Vision trades. both layouts are seven columns and
  //    open with id,price,qty; the timestamp and the boolean sit at different
  //    positions:
  //      trades   : id,price,qty,quote_qty,time,is_buyer_maker,is_best_match
  //      aggTrades: id,price,qty,first_id,last_id,time,is_buyer_maker
  if (n == 7 && csv_detail::LooksNumeric(fields[1]) &&
      csv_detail::LooksNumeric(fields[2])) {
    if (LooksBoolean(fields[5])) {
      cols.ts = 4;
      cols.price = 1;
      cols.qty = 2;
      cols.maker = 5;
      return cols;
    }
    if (LooksBoolean(fields[6])) {
      cols.ts = 5;
      cols.price = 1;
      cols.qty = 2;
      cols.maker = 6;
      return cols;
    }
  }

  // 3) headerless Bybit public trades. the linear feed is
  //    timestamp,symbol,side,size,price,...; the spot export puts the side
  //    after price/size instead, so both shapes are accepted.
  if (n >= 5 && LooksTimestamp(fields[0]) &&
      !csv_detail::LooksNumeric(fields[1])) {
    if (LooksSideWord(fields[2]) && csv_detail::LooksNumeric(fields[3]) &&
        csv_detail::LooksNumeric(fields[4])) {
      cols.ts = 0;
      cols.side = 2;
      cols.qty = 3;
      cols.price = 4;
      return cols;
    }
    if (csv_detail::LooksNumeric(fields[2]) &&
        csv_detail::LooksNumeric(fields[3]) && LooksSideWord(fields[4])) {
      cols.ts = 0;
      cols.price = 2;
      cols.qty = 3;
      cols.side = 4;
      return cols;
    }
  }

  // 4) headerless Hyperliquid fills: coin,side,px,sz,time,hash,tid,...
  if (n >= 5 && !csv_detail::LooksNumeric(fields[0]) &&
      LooksSideWord(fields[1]) && csv_detail::LooksNumeric(fields[2]) &&
      csv_detail::LooksNumeric(fields[3]) && LooksTimestamp(fields[4])) {
    cols.ts = 4;
    cols.price = 2;
    cols.qty = 3;
    cols.side = 1;
    return cols;
  }

  // 5) unknown layout: leave the indices at -1 for the caller to reject.
  return cols;
}

CsvColumns CSVLoader::SniffL2Columns(const std::string &first_line) {
  CsvColumns cols;
  if (first_line.empty()) return cols;

  const std::vector<std::string> fields = csv_detail::SplitLine(first_line);

  // 1) labelled header. every bid/ask price+qty column is collected in file
  // order, so multi-level book files ("bid_price_1", "bid_price_2", ...) yield
  // one vector entry per depth level.
  bool any_label = false;
  for (size_t i = 0; i < fields.size(); ++i) {
    const std::string n = NormalizeLabel(fields[i]);
    if (n.empty()) continue;

    const bool bid_side = IsBidSideLabel(n);
    const bool ask_side = IsAskSideLabel(n);
    if (!bid_side && !ask_side) continue;

    const bool price = IsPricePart(n);
    const bool qty = IsQtyPart(n);
    if (!price && !qty) continue;

    any_label = true;
    const int idx = static_cast<int>(i);
    if (bid_side) {
      if (price) cols.bid_px.push_back(idx);
      else cols.bid_qty.push_back(idx);
    } else {
      if (price) cols.ask_px.push_back(idx);
      else cols.ask_qty.push_back(idx);
    }
  }
  if (any_label) {
    cols.has_header = true;
    return cols;
  }

  // a headerless Binance trades row also carries numeric values in 1..4. its
  // trailing true/false is a reliable tell, so a trades file is not mistaken
  // for a book (the numeric event_time of a bookTicker never matches this).
  if (fields.size() == 7 && LooksBoolWord(fields[6])) return cols;

  // 2) headerless: canonical Binance Vision bookTicker,
  //    update_id,bid_px,bid_qty,ask_px,ask_qty,transact_time,event_time
  if (fields.size() >= 5 && csv_detail::LooksNumeric(fields[1]) &&
      csv_detail::LooksNumeric(fields[2]) &&
      csv_detail::LooksNumeric(fields[3]) &&
      csv_detail::LooksNumeric(fields[4])) {
    cols.bid_px = {1};
    cols.bid_qty = {2};
    cols.ask_px = {3};
    cols.ask_qty = {4};
    return cols;
  }

  // 3) unknown layout: leave the vectors empty for the caller to reject.
  return cols;
}
