#include "mokuren.hpp"

#include "data_manager.hpp"
#include "logger.hpp"
#include "replay_manager.hpp"

#include <string>
#include <utility>
#include <vector>


void Mokuren::RunGridSearch(const std::string &/*ignored_legacy_path*/,
                            const std::string &config_path,
                            std::function<void(const std::string&)> on_status,
                            std::function<void(int, int)> on_progress,
                            std::function<void(const TestResult&)> on_result) {
    grid_runner_.Run(config_path, std::move(on_status),
                     std::move(on_progress), std::move(on_result));
}

void Mokuren::RunWFA(const std::string &/*ignored_legacy_path*/,
                     const std::string &config_path,
                     std::function<void(const std::string&)> on_status,
                     std::function<void(int, int)> on_progress,
                     std::function<void(const TestResult&)> on_result) {
    wfa_runner_.RunWFA(config_path, std::move(on_status),
                       std::move(on_progress), std::move(on_result));
}

    

    


ReplayResult Mokuren::ReplaySingleRun(const std::string &/*ignored*/, const nlohmann::json& winning_config) {
    ReplayResult result{};

    std::string active_dataset = winning_config.value("environment", nlohmann::json::object()).value("dataset", "data");
    LOG_INFO("replay single run: dataset={}", active_dataset);
    std::vector<TradeEvent> all_trades = DataManager::LoadAllTrades(active_dataset);
    if (all_trades.empty()) {
        LOG_ERROR("replay single run: no trades found in dataset {}", active_dataset);
        return result;
    }

    // ReplayManager::Run is the single replay implementation: it owns the
    // stop-validation/sizing/firewall/TP funnel, the L2 catch-up and the
    // candle mapping, so this entry point cannot drift from it again.
    result = ReplayManager::Run(all_trades, winning_config);

    LOG_INFO("replay single run complete: bars_1m={} bars_15m={} trades={}",
             result.history_1m.size(), result.history_15m.size(), result.trades.size());

    return result;
}

ReplayResult Mokuren::ReplayWFA(const std::string &/*ignored_legacy_path*/,
                                const std::vector<WFAOOSConfig>& wfa_configs) {
    return wfa_runner_.ReplayWFA(wfa_configs);
}
