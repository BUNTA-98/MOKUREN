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
    // `dataset_path` is either the historical directory layout (trades.csv /
    // L2.csv, cached as .bin next to them) or a single .mkn recording written
    // by mokuren_record. A recording carries both channels and its own
    // metadata, so both loaders read the same file path and neither builds a
    // side cache for it.
    //
    // Both THROW std::runtime_error when a recording decoded with an error, a
    // block was unusable, or records had to be dropped. A recording with
    // skipped book blocks is not a shorter dataset, it is a book-blind one --
    // and a caller that got a vector back would run on it and report "no
    // setups" on a market that was moving. The message names the counts, so it
    // doubles as the diagnosis. An EMPTY channel is still returned as-is: a
    // trade-only recording is a legitimate dataset.
    static std::vector<TradeEvent> LoadAllTrades(const std::string& dataset_path);
    static std::vector<L2Snapshot> LoadAllL2Snapshots(const std::string& dataset_path);

    // Both channels from a single pass over the dataset. A .mkn recording is
    // one file, so calling the two loaders above decoded it twice: twice the
    // I/O, and on a recording that is still being written the second pass can
    // see more blocks than the first -- pairing a trade vector with an L2
    // vector from a different moment of the file. Callers that need both
    // channels must use this.
    struct LoadedDataset {
        std::vector<TradeEvent> trades;
        std::vector<L2Snapshot> l2;
    };
    static LoadedDataset LoadAll(const std::string& dataset_path);
    
    static bool ValidateDataAlignment(const std::vector<TradeEvent>& trades, const std::vector<L2Snapshot>& l2, std::function<void(const std::string&)> on_status);
    
    static std::vector<TradeEvent> SliceTrades(const std::vector<TradeEvent>& source_trades, int64_t start_time, int64_t end_time);
    static std::vector<L2Snapshot> SliceL2(const std::vector<L2Snapshot>& source_l2, int64_t start_time, int64_t end_time);
    static std::vector<WFAWindow> GenerateWFAWindows(int64_t first_ts, int64_t last_ts, int64_t in_sample_ms, int64_t out_of_sample_ms, int64_t step_ms);

private:
    // internal fast dyn l2 csv to bin parser
    static bool ConvertL2CsvToBin(const std::string& csv_path, const std::string& bin_path);
};
