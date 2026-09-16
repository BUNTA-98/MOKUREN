#include <iostream>
#include <cstdint>
#include <cassert>
// Hier deine bestehenden Structs (TradeEvent, PriceLevel, Bar, ExecutionSignal) einbinden
#include "trade_event.hpp"
#include "analyzer.hpp"



int main() {
  
    Analyzer analyzer{};
    Bar bar{};
    bar.tick_size = 0.5;
    bar.base_price = 0;

    constexpr int64_t INTERVAL_NS = 60'000'000'000LL; // 60 Sekunden Intervall

    std::cout << "=== STARTING FOOTPRINT CORE TEST ===" << std::endl;

    // 1. Erste Kerze befüllen (Simulierte Trades)
    // Ticks: Price, Quantity, is_market_buy
    TradeEvent t1{ 1'000'000'000LL, 100.0, 1.0, false }; // Market Sell @ 100.0
    TradeEvent t2{ 2'000'000'000LL, 101.0, 5.0, true  }; // Market Buy  @ 101.0
    TradeEvent t3{ 3'000'000'000LL, 102.0, 8.0, true  }; // Market Buy  @ 102.0
    TradeEvent t4{ 4'000'000'000LL, 99.0,  2.0, false }; // Market Sell @ 99.0

    // Trades verarbeiten (sollten alle Signal::NONE liefern)
    assert(analyzer.ProcessTrade(bar, t1, INTERVAL_NS) == eSignal::NONE);
    analyzer.ProcessTrade(bar, t2, INTERVAL_NS);
    analyzer.ProcessTrade(bar, t3, INTERVAL_NS);
    analyzer.ProcessTrade(bar, t4, INTERVAL_NS);

    // 2. Zustand der aktiven Kerze prüfen
    std::cout << "[Test 1] OHLC & Volumina:" << std::endl;
    std::cout << "  Open : " << bar.open  << " (Erwartet: 100.0)" << std::endl;
    std::cout << "  High : " << bar.high  << " (Erwartet: 102.0)" << std::endl;
    std::cout << "  Low  : " << bar.low   << " (Erwartet: 99.0)"  << std::endl;
    std::cout << "  Close: " << bar.close << " (Erwartet: 99.0)"  << std::endl;
    std::cout << "  Total Vol: " << bar.total_volume << " (Erwartet: 16.0)" << std::endl;
    std::cout << "  Active Levels: " << bar.active_levels << " (Erwartet: 4)" << std::endl;

    // 3. Timeframe Rollover testen (Trade nach 60 Sekunden schickt alte Kerze in analyzeCandle)
    TradeEvent t_rollover{ 61'000'000'000LL, 99.5, 1.0, true };
    eSignal signal = analyzer.ProcessTrade(bar, t_rollover, INTERVAL_NS);

    std::cout << "\n[Test 2] Timeframe Rollover:" << std::endl;
    std::cout << "  Signal empfangen: " << (signal == eSignal::NONE ? "NONE" : "SIGNAL") << std::endl;
    std::cout << "  Alte POC Price  : " << bar.poc_price << std::endl;
    std::cout << "  Neue Kerze Open : " << bar.open << " (Erwartet: 99.5)" << std::endl;
    std::cout << "  Neue Kerze Active Levels: " << bar.active_levels << " (Erwartet: 1)" << std::endl;

    std::cout << "\n=== ALL TESTS PASSED ===" << std::endl;
    return 0;
}
