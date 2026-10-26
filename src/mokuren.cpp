#include "mokuren.hpp"

#include "data_manager.hpp"
#include "logger.hpp"
#include "replay_manager.hpp"

#include <exception>
#include <string>
#include <utility>
#include <vector>


void Mokuren::RunGridSearch(const std::string &dataset_path,
                            const std::string &config_path,
                            std::function<void(const std::string&)> on_status,
                            std::function<void(int, int)> on_progress,
                            std::function<void(const TestResult&)> on_result) {
    // forward the resolved dataset: the runner falls back to the config only
    // when this is empty.
    grid_runner_.Run(config_path, std::move(on_status),
                     std::move(on_progress), std::move(on_result),
                     dataset_path);
}

void Mokuren::RunWFA(const std::string &dataset_path,
                     const std::string &config_path,
                     std::function<void(const std::string&)> on_status,
                     std::function<void(int, int)> on_progress,
                     std::function<void(const TestResult&)> on_result) {
    wfa_runner_.RunWFA(config_path, std::move(on_status),
                       std::move(on_progress), std::move(on_result),
                       dataset_path);
}

    

    


ReplayResult Mokuren::ReplaySingleRun(const std::string &dataset_path, const nlohmann::json& winning_config) {
    ReplayResult result{};

    // the path the operator selected wins; only a caller with no profile in
    // hand (CLI, tests) falls back to the config.
    std::string active_dataset = dataset_path;
    if (active_dataset.empty()) {
        active_dataset = winning_config.value("environment", nlohmann::json::object()).value("dataset", "data");
    }
    LOG_INFO("replay single run: dataset={}", active_dataset);

    // one guard around both channels: a damaged recording throws from either
    // load, or from ReplayManager once it reads the book. letting that unwind
    // would take the TUI down instead of showing the reason.
    try {
        // one decode of the recording. ReplayManager used to load the book a
        // second time from the same path, which cost a second pass over a
        // multi-gigabyte file and let the two channels come from different
        // states of a recording that was still being written.
        DataManager::LoadedDataset loaded = DataManager::LoadAll(active_dataset);
        if (loaded.trades.empty()) {
            LOG_ERROR("replay single run: no trades found in dataset {}", active_dataset);
            return result;
        }

        // ReplayManager::Run is the single replay implementation: it owns the
        // stop-validation/sizing/firewall/TP funnel, the L2 catch-up and the
        // candle mapping, so this entry point cannot drift from it again. it
        // loads the book from the SAME path, not from the config.
        result = ReplayManager::Run(loaded.trades, loaded.l2, winning_config,
                                    active_dataset);
    } catch (const std::exception& e) {
        LOG_ERROR("replay single run: {}", e.what());
        return result;
    }

    LOG_INFO("replay single run complete: bars_1m={} bars_15m={} trades={}",
             result.history_1m.size(), result.history_15m.size(), result.trades.size());

    return result;
}

ReplayResult Mokuren::ReplayWFA(const std::string &dataset_path,
                                const std::vector<WFAOOSConfig>& wfa_configs) {
    return wfa_runner_.ReplayWFA(wfa_configs, dataset_path);
}
