#include "data_manager.hpp"
#include "csv_parser.hpp"
#include "timestamp_utils.hpp"
#include <filesystem>
#include <system_error>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace fs = std::filesystem;

namespace {

// Reads a whole L2 cache. false means missing, truncated, or carrying a partial
// trailing record. The caller drops the file and rebuilds from the CSV, so a
// truncated cache self-heals in the same run instead of one run later.
bool ReadL2Cache(const fs::path &bin_file, std::vector<L2Snapshot> &out) {
    std::ifstream file(bin_file.string(), std::ios::binary | std::ios::ate);
    if (!file) return false;

    const std::streamsize size = file.tellg();
    if (size <= 0 || size % static_cast<std::streamsize>(sizeof(L2Snapshot)) != 0) {
        return false;
    }

    file.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size) / sizeof(L2Snapshot));
    if (!file.read(reinterpret_cast<char *>(out.data()), size)) {
        out.clear();
        return false;
    }
    return true;
}

// Bybit and Hyperliquid publish the taker side instead of Binance's
// is_buyer_maker bit. Bybit exports "Buy"/"Sell", Hyperliquid "B"/"A"; both
// name the aggressor, so a taker sell means the resting bid was hit and the
// buyer was the maker.
bool IsBuyerMakerFromSideToken(const char *token) {
    if (token == nullptr) return false;

    std::string s;
    for (const char *p = token; *p; ++p) {
        if (*p == ' ' || *p == '\t' || *p == '"' || *p == '\r') continue;
        s.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(*p))));
    }
    return s == "a" || s == "s" || s == "sell" || s == "ask" ||
           s == "short" || s == "-1" || s == "false";
}

} // namespace

bool DataManager::ConvertL2CsvToBin(const std::string& csv_path, const std::string& bin_path) {
    std::ifstream csv_file(csv_path);
    if (!csv_file.is_open()) return false;

    std::string line;
    if (!std::getline(csv_file, line)) return false; 

    // one sniffer shared with LoadAllTrades and csv_parser: a file that starts
    // directly with a snapshot is read with the canonical Binance Vision
    // bookTicker layout instead of being rejected as "no header".
    const CsvColumns cols = CSVLoader::SniffL2Columns(line);
    const int ts_idx = cols.ts;
    const auto &bids_p = cols.bid_px;
    const auto &bids_q = cols.bid_qty;
    const auto &asks_p = cols.ask_px;
    const auto &asks_q = cols.ask_qty;

    if (ts_idx == -1) return false;

    size_t max_csv_depth = std::min({bids_p.size(), bids_q.size(), asks_p.size(), asks_q.size()});
    L2Snapshot dummy{};
    size_t struct_max_depth = sizeof(dummy.bids) / sizeof(dummy.bids[0]);
    size_t safe_depth = std::min(max_csv_depth, struct_max_depth);

    // a recognised L2 file that maps to zero levels used to be written out as
    // zero-filled snapshots: valid-looking rows with an empty book, which
    // silently disabled every book trigger. refuse instead of poisoning the
    // cache. the output file is opened only past this point, so a failed
    // conversion leaves nothing behind for the next run to mistake for data.
    if (safe_depth == 0) return false;

    std::ofstream bin_file(bin_path, std::ios::binary);
    if (!bin_file.is_open()) return false;

    std::vector<char*> tokens;
    tokens.reserve(32);

    bool first_is_data = !cols.has_header;
    while (first_is_data || std::getline(csv_file, line)) {
        if (first_is_data) {
            first_is_data = false;
        } else if (line.empty()) {
            continue;
        }

        tokens.clear();
        char* ptr = line.data();
        tokens.push_back(ptr);
        
        while (*ptr) {
            if (*ptr == ',') {
                *ptr = '\0'; 
                tokens.push_back(ptr + 1);
            }
            ptr++;
        }

        if (tokens.size() <= static_cast<size_t>(ts_idx)) continue;

        L2Snapshot snap{};
        // exports arrive in s/ms/us/ns depending on the tool; normalise once
        // here so a microsecond file is not read as a date in the year 55000.
        snap.timestamp = NormalizeEpochToMillis(std::strtoll(tokens[ts_idx], nullptr, 10));
        
        if (snap.timestamp < 1500000000000LL) continue; 

        for (size_t i = 0; i < safe_depth; ++i) {
            if (tokens.size() > bids_p[i] && *tokens[bids_p[i]]) snap.bids[i].price = std::atof(tokens[bids_p[i]]);
            if (tokens.size() > bids_q[i] && *tokens[bids_q[i]]) snap.bids[i].qty = std::atof(tokens[bids_q[i]]);
            if (tokens.size() > asks_p[i] && *tokens[asks_p[i]]) snap.asks[i].price = std::atof(tokens[asks_p[i]]);
            if (tokens.size() > asks_q[i] && *tokens[asks_q[i]]) snap.asks[i].qty = std::atof(tokens[asks_q[i]]);
        }

        if (safe_depth > 0) {
            snap.best_bid_price = snap.bids[0].price;
            snap.best_bid_qty = snap.bids[0].qty;
            snap.best_ask_price = snap.asks[0].price;
            snap.best_ask_qty = snap.asks[0].qty;
        }

        bin_file.write(reinterpret_cast<const char*>(&snap), sizeof(L2Snapshot));
    }
    return true;
}

std::vector<TradeEvent> DataManager::LoadAllTrades(const std::string &dataset_path) {
    fs::path path = dataset_path;
    fs::path csv_file = path / "trades.csv";
    
    // v2: timestamps are normalised to milliseconds before being cached, so a
    // v1 cache would be replayed at the wrong scale.
    fs::path cache_path = path / "trades_cache_v2.bin"; 

    // try load bin cache
    if (fs::exists(cache_path)) {
        std::ifstream cache_file(cache_path, std::ios::binary);
        if (cache_file) {
            cache_file.seekg(0, std::ios::end);
            std::streamsize size = cache_file.tellg();
            cache_file.seekg(0, std::ios::beg);

            // only whole records are valid: an interrupted write leaves a
            // partial trailing record, and reading `size` bytes into a
            // count-sized vector overran it by the remainder. a bad cache
            // simply falls through to the CSV below.
            if (size > 0 && size % static_cast<std::streamsize>(sizeof(TradeEvent)) == 0) {
                std::vector<TradeEvent> cached(static_cast<size_t>(size) / sizeof(TradeEvent));
                if (cache_file.read(reinterpret_cast<char*>(cached.data()), size)) {
                    return cached;
                }
            }
        }
    }

    std::vector<TradeEvent> all_trades;
    all_trades.reserve(5000000); 

    std::ifstream file(csv_file.string());
    if (file.is_open()) {
        std::string line;
        if (std::getline(file, line)) {
            // the same sniffer csv_parser uses: headerless exports are read as
            // Binance Vision aggTrades instead of yielding no trades at all.
            const CsvColumns cols = CSVLoader::SniffTradeColumns(line);
            const int ts_idx = cols.ts, p_idx = cols.price,
                      q_idx = cols.qty, m_idx = cols.maker,
                      s_idx = cols.side;

            // bybit and hyperliquid carry the taker side instead of the maker
            // bit, so either column satisfies the layout check.
            if (ts_idx != -1 && p_idx != -1 && q_idx != -1 &&
                (m_idx != -1 || s_idx != -1)) {
                const size_t max_idx = static_cast<size_t>(
                    std::max({ts_idx, p_idx, q_idx, m_idx, s_idx}));

                std::vector<char*> tokens;
                tokens.reserve(32);

                auto process_row = [&](std::string &row) {
                    if (row.empty()) return;

                    tokens.clear();
                    char* ptr = row.data();
                    tokens.push_back(ptr);

                    while (*ptr) {
                        if (*ptr == ',') {
                            *ptr = '\0';
                            tokens.push_back(ptr + 1);
                        }
                        ptr++;
                    }

                    if (tokens.size() <= max_idx) return;

                    TradeEvent trade{};
                    trade.timestamp = NormalizeEpochToMillis(
                        std::strtoll(tokens[ts_idx], nullptr, 10));

                    if (trade.timestamp > 1500000000000LL) {
                        trade.price = std::atof(tokens[p_idx]);
                        trade.quantity = std::atof(tokens[q_idx]);

                        if (m_idx >= 0) {
                            char* m_ptr = tokens[m_idx];
                            while (*m_ptr == ' ' || *m_ptr == '\t' || *m_ptr == '"') m_ptr++;
                            trade.is_buyer_maker =
                                (*m_ptr == 't' || *m_ptr == 'T' || *m_ptr == '1');
                        } else {
                            trade.is_buyer_maker =
                                IsBuyerMakerFromSideToken(tokens[s_idx]);
                        }

                        all_trades.push_back(trade);
                    }
                };

                if (!cols.has_header) process_row(line);
                while (std::getline(file, line)) process_row(line);
            }
        }
    }

    // save cache
    if (!all_trades.empty()) {
        std::ofstream cache_file(cache_path, std::ios::binary);
        if (cache_file) {
            cache_file.write(reinterpret_cast<const char*>(all_trades.data()), all_trades.size() * sizeof(TradeEvent));
        }
    }
    return all_trades;
}



std::vector<L2Snapshot> DataManager::LoadAllL2Snapshots(const std::string &dataset_path) {
    fs::path path = dataset_path;
    // bumped to v3 together with the L2Snapshot layout change (dropping the
    // pragma pack widened a record from 200 to 688 bytes). a stale v2 cache
    // must never be read at the new stride.
    fs::path bin_file = path / "L2_cache_v3.bin";
    fs::path csv_file = path / "L2.csv"; 
    
    std::vector<L2Snapshot> l2_data;

    if (!fs::exists(bin_file) && fs::exists(csv_file)) {
        ConvertL2CsvToBin(csv_file.string(), bin_file.string());
    }
    
    if (fs::exists(bin_file)) {
        if (ReadL2Cache(bin_file, l2_data)) return l2_data;

        // truncated by an interrupted write (power loss, kill -9). drop it and
        // rebuild immediately: returning an empty book here made the first run
        // after a crash look like a quiet market and only healed on the next.
        std::error_code ec;
        fs::remove(bin_file, ec);
        l2_data.clear();

        if (fs::exists(csv_file) &&
            ConvertL2CsvToBin(csv_file.string(), bin_file.string()) &&
            ReadL2Cache(bin_file, l2_data)) {
            return l2_data;
        }
    }
    return l2_data;
}

bool DataManager::ValidateDataAlignment(const std::vector<TradeEvent>& trades, const std::vector<L2Snapshot>& l2, std::function<void(const std::string&)> on_status) {
    if (trades.empty() || l2.empty()) return true; 
    
    int64_t t_start = trades.front().timestamp;
    int64_t l2_start = l2.front().timestamp;
    int64_t diff_start_min = std::abs(t_start - l2_start) / 60000LL;

    int64_t t_end = trades.back().timestamp;
    int64_t l2_end = l2.back().timestamp;
    int64_t diff_end_min = std::abs(t_end - l2_end) / 60000LL;

    if (diff_start_min > 60) {
        on_status("ERR: DATA DESYNC START (DIFF " + std::to_string(diff_start_min) + "m)");
        return false;
    }
    
    if (diff_end_min > 60) {
        on_status("ERR: DATA DESYNC END (DIFF " + std::to_string(diff_end_min) + "m)");
        return false;
    }
    
    return true;
}

std::vector<TradeEvent> DataManager::SliceTrades(const std::vector<TradeEvent>& source_trades, int64_t start_time, int64_t end_time) {
    std::vector<TradeEvent> sliced;
    sliced.reserve(source_trades.size() / 4); 

    for (const auto& trade : source_trades) {
        if (trade.timestamp >= start_time && trade.timestamp <= end_time) {
            sliced.push_back(trade);
        }
        if (trade.timestamp > end_time) break; 
    }
    return sliced;
}

std::vector<L2Snapshot> DataManager::SliceL2(const std::vector<L2Snapshot>& source_l2, int64_t start_time, int64_t end_time) {
    std::vector<L2Snapshot> sliced;
    sliced.reserve(source_l2.size() / 4); 

    for (const auto& l2 : source_l2) {
        if (l2.timestamp >= start_time && l2.timestamp <= end_time) {
            sliced.push_back(l2);
        }
        if (l2.timestamp > end_time) break; 
    }
    return sliced;
}

std::vector<WFAWindow> DataManager::GenerateWFAWindows(int64_t first_ts, int64_t last_ts, int64_t in_sample_ms, int64_t out_of_sample_ms, int64_t step_ms) {
    std::vector<WFAWindow> windows;
    int64_t current_start = first_ts;

    while (true) {
        WFAWindow win;
        win.in_sample_start = current_start;
        win.in_sample_end = current_start + in_sample_ms - 1;
        
        win.out_of_sample_start = win.in_sample_end + 1;
        win.out_of_sample_end = win.out_of_sample_start + out_of_sample_ms - 1;

        if (win.out_of_sample_end > last_ts) break; 

        windows.push_back(win);
        current_start += step_ms; 
    }
    return windows;
}
