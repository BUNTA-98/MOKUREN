#include "performance_metrics.hpp"
#include <cmath>
#include <random>
#include <algorithm>

AdvancedMetrics PerformanceMetrics::CalculateAdvancedMetrics(const std::vector<TradeRecord>& trades) {
    AdvancedMetrics m;
    if (trades.size() < 2) return m;

    double sum_return = 0.0;
    std::vector<double> returns;
    for (const auto& t : trades) {
        returns.push_back(t.net_profit);
        sum_return += t.net_profit;
    }
    double mean = sum_return / returns.size();

    double var = 0.0, down_var = 0.0;
    for (double r : returns) {
        var += (r - mean) * (r - mean);
        if (r < 0) down_var += r * r; 
    }
    var /= returns.size();
    down_var /= returns.size();

    double std_dev = std::sqrt(var);
    double down_dev = std::sqrt(down_var);

    if (std_dev > 0.0001) m.sharpe = (mean / std_dev) * std::sqrt(trades.size());
    if (down_dev > 0.0001) m.sortino = (mean / down_dev) * std::sqrt(trades.size());

    std::mt19937 rng(42); 
    std::vector<double> sim = returns;
    double worst_dd = 0.0;
    for (int i = 0; i < 1000; i++) {
        std::shuffle(sim.begin(), sim.end(), rng);
        double peak = 10000.0, balance = 10000.0, max_dd = 0.0;
        
        for (double r : sim) {
            balance += r;
            if (balance > peak) peak = balance;
            double dd = (peak - balance) / peak * 100.0;
            if (dd > max_dd) max_dd = dd;
        }
        if (max_dd > worst_dd) worst_dd = max_dd;
    }
    m.mc_dd = worst_dd;

    return m;
}