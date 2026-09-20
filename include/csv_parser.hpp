#pragma once
#include <string>
#include <functional>
#include "market_context.hpp" // Nur für das TradeEvent struct benötigt

class CSVLoader {
public:
    // Nimmt den Dateipfad und das Callback (die auszuführende Aktion pro Trade)
    static size_t ProcessBinanceCSV(
        const std::string& filepath, 
        std::function<void(const TradeEvent&)> on_trade
    );
};
