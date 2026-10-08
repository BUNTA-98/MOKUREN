#include "data_manager.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace fs = std::filesystem;

bool DataManager::ConvertL2CsvToBin(const std::string& csv_path, const std::string& bin_path) {
    std::ifstream csv_file(csv_path);
    if (!csv_file.is_open()) return false;

    std::ofstream bin_file(bin_path, std::ios::binary);
    if (!bin_file.is_open()) return false;

    std::string line;
    if (!std::getline(csv_file, line)) return false; 

    std::vector<std::string> headers;
    size_t start = 0, end = line.find(',');
    while (end != std::string::npos) {
        headers.push_back(line.substr(start, end - start));
        start = end + 1;
        end = line.find(',', start);
    }
    headers.push_back(line.substr(start));

    int ts_idx = -1;
    std::vector<int> bids_p, bids_q, asks_p, asks_q;

    for (size_t i = 0; i < headers.size(); ++i) {
        std::string h = headers[i];
        std::transform(h.begin(), h.end(), h.begin(), ::tolower);
        
        // Findet transaction_time, event_time oder timestamp
        if (h.find("time") != std::string::npos || h.find("ts") != std::string::npos) {
            if (ts_idx == -1 || h.find("transaction") != std::string::npos) ts_idx = i; 
        }
        // Identifiziert bookTicker Bids und Asks
        else if (h.find("bid") != std::string::npos && (h.find("price") != std::string::npos || h == "p" || h.find("p") != std::string::npos)) bids_p.push_back(i);
        else if (h.find("bid") != std::string::npos && (h.find("qty") != std::string::npos || h.find("quantity") != std::string::npos || h == "q")) bids_q.push_back(i);
        else if (h.find("ask") != std::string::npos && (h.find("price") != std::string::npos || h == "p" || h.find("p") != std::string::npos)) asks_p.push_back(i);
        else if (h.find("ask") != std::string::npos && (h.find("qty") != std::string::npos || h.find("quantity") != std::string::npos || h == "q")) asks_q.push_back(i);
    }

    if (ts_idx == -1) return false;

    size_t max_csv_depth = std::min({bids_p.size(), bids_q.size(), asks_p.size(), asks_q.size()});
    L2Snapshot dummy{};
    size_t struct_max_depth = sizeof(dummy.bids) / sizeof(dummy.bids[0]);
    size_t safe_depth = std::min(max_csv_depth, struct_max_depth);

    std::vector<char*> tokens;
    tokens.reserve(headers.size());

    while (std::getline(csv_file, line)) {
        if (line.empty()) continue; 

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
        snap.timestamp = std::strtoll(tokens[ts_idx], nullptr, 10);
        
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
    
    // V3 zwingt die Engine, den alten Cache komplett zu ignorieren!
    fs::path cache_path = path / "trades_cache_v3.bin"; 

    // try load bin cache
    if (fs::exists(cache_path)) {
        std::ifstream cache_file(cache_path, std::ios::binary);
        if (cache_file) {
            cache_file.seekg(0, std::ios::end);
            std::streamsize size = cache_file.tellg();
            cache_file.seekg(0, std::ios::beg);

            size_t count = size / sizeof(TradeEvent);
            std::vector<TradeEvent> all_trades(count);

            if (cache_file.read(reinterpret_cast<char*>(all_trades.data()), size)) {
                return all_trades;
            }
        }
    }

    std::vector<TradeEvent> all_trades;
    all_trades.reserve(5000000); 

    std::ifstream file(csv_file.string());
    if (file.is_open()) {
        std::string line;
        if (std::getline(file, line)) {
            // parse headers
            std::vector<std::string> headers;
            size_t start = 0, end = line.find(',');
            while (end != std::string::npos) {
                headers.push_back(line.substr(start, end - start));
                start = end + 1;
                end = line.find(',', start);
            }
            headers.push_back(line.substr(start));

            // Entfernt eventuelle \r Zeilenumbrüche am Ende des Headers
            if (!headers.empty() && !headers.back().empty() && headers.back().back() == '\r') {
                headers.back().pop_back();
            }

            int ts_idx = -1, p_idx = -1, q_idx = -1, m_idx = -1;
            for (size_t i = 0; i < headers.size(); ++i) {
                std::string h = headers[i];
                std::transform(h.begin(), h.end(), h.begin(), ::tolower);
                
                // EXAKTE MATCHES statt find() - so ignorieren wir 'quote_qty' sicher!
                if (h == "time" || h == "ts") ts_idx = i;
                else if (h == "price" || h == "p") p_idx = i;
                else if (h == "qty" || h == "quantity" || h == "q") q_idx = i; 
                else if (h == "is_buyer_maker" || h == "maker" || h == "m") m_idx = i;
            }

            if (ts_idx != -1 && p_idx != -1 && q_idx != -1 && m_idx != -1) {
                std::vector<char*> tokens;
                tokens.reserve(headers.size());
                
                while (std::getline(file, line)) {
                    if (line.empty()) continue;
                    
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

                    if (tokens.size() <= static_cast<size_t>(std::max({ts_idx, p_idx, q_idx, m_idx}))) continue;

                    TradeEvent trade{};
                    trade.timestamp = std::strtoll(tokens[ts_idx], nullptr, 10);
                    
                    if (trade.timestamp > 1500000000000LL) {
                        trade.price = std::atof(tokens[p_idx]);
                        
                        // Hier wieder quantity (oder wie auch immer dein Struct-Feld heißt)
                        trade.quantity = std::atof(tokens[q_idx]); 
                        
                        char* m_ptr = tokens[m_idx];
                        while (*m_ptr == ' ' || *m_ptr == '\t' || *m_ptr == '"') m_ptr++;
                        trade.is_buyer_maker = (*m_ptr == 't' || *m_ptr == 'T' || *m_ptr == '1');
                        
                        all_trades.push_back(trade);
                    }
                }
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
    // Der L2 Cache wird ebenfalls neu forciert, da sich das CSV Format geaendert hat!
    fs::path bin_file = path / "L2_cache_v2.bin"; 
    fs::path csv_file = path / "L2.csv"; 
    
    std::vector<L2Snapshot> l2_data;

    if (!fs::exists(bin_file) && fs::exists(csv_file)) {
        ConvertL2CsvToBin(csv_file.string(), bin_file.string());
    }
    
    if (fs::exists(bin_file)) {
        std::ifstream file(bin_file.string(), std::ios::binary | std::ios::ate);
        if (file) {
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            
            l2_data.resize(size / sizeof(L2Snapshot));
            if (file.read(reinterpret_cast<char*>(l2_data.data()), size)) {
                return l2_data;
            }
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
