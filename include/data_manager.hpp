#pragma once
// the three types this header needs: TradeEvent/L2Snapshot live in
// trade_event.hpp, WFAWindow in wfa_window.hpp. including mokuren.hpp pulled in
// the whole engine and closed a cycle -- mokuren.hpp -> replay_manager.hpp ->
// data_manager.hpp -> mokuren.hpp -- which left WFAWindow (or DataManager)
// undeclared depending on which translation unit entered the cycle first.
#include "trade_event.hpp"
#include "wfa_window.hpp"
#include <vector>
#include <string>
#include <functional>

class DataManager {
public:
    static std::vector<TradeEvent> LoadAllTrades(const std::string& dataset_path);
    static std::vector<L2Snapshot> LoadAllL2Snapshots(const std::string& dataset_path);
    
    static bool ValidateDataAlignment(const std::vector<TradeEvent>& trades, const std::vector<L2Snapshot>& l2, std::function<void(const std::string&)> on_status);
    
    static std::vector<TradeEvent> SliceTrades(const std::vector<TradeEvent>& source_trades, int64_t start_time, int64_t end_time);
    static std::vector<L2Snapshot> SliceL2(const std::vector<L2Snapshot>& source_l2, int64_t start_time, int64_t end_time);
    static std::vector<WFAWindow> GenerateWFAWindows(int64_t first_ts, int64_t last_ts, int64_t in_sample_ms, int64_t out_of_sample_ms, int64_t step_ms);

private:
    // internal fast dyn l2 csv to bin parser
    static bool ConvertL2CsvToBin(const std::string& csv_path, const std::string& bin_path);
};
