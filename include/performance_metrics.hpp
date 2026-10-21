#pragma once
#include "test_result.hpp"
#include <vector>

namespace PerformanceMetrics {
    AdvancedMetrics CalculateAdvancedMetrics(const std::vector<TradeRecord>& trades);
}
