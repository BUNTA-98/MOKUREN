#pragma once
#include "mokuren.hpp" 
#include <vector>

namespace PerformanceMetrics {
    AdvancedMetrics CalculateAdvancedMetrics(const std::vector<TradeRecord>& trades);
}