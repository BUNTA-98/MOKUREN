#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "mokuren.hpp"
#include <vector>
#include <string>
#include "data_manager.hpp"

TEST_CASE("Mokuren Data Alignment Validation", "[mokuren][alignment]") {
    Mokuren mokuren;
    
    // Wir fangen die Konsolen-Ausgabe (on_status) ein, um die Fehlermeldungen zu prüfen
    std::string last_status_msg = "";
    auto on_status = [&](const std::string& msg) {
        last_status_msg = msg;
    };

    // Hilfs-Konstanten für die Zeit in Millisekunden
    const int64_t MINUTE_MS = 60000LL;
    const int64_t HOUR_MS = 60 * MINUTE_MS;
    const int64_t DAY_MS = 24 * HOUR_MS;

    SECTION("Leere Datensaetze werden toleriert (Early Exit)") {
        std::vector<TradeEvent> trades;
        std::vector<L2Snapshot> l2;
        
        REQUIRE(DataManager::ValidateDataAlignment(trades, l2, on_status) == true);
    }

    SECTION("Perfekt synchronisierte Daten gehen durch") {
        TradeEvent t1, t2;
        t1.timestamp = 1000000;
        t2.timestamp = 5000000;
        
        L2Snapshot l1, l2_snap;
        l1.timestamp = 1000000;
        l2_snap.timestamp = 5000000;

        std::vector<TradeEvent> trades = {t1, t2};
        std::vector<L2Snapshot> l2 = {l1, l2_snap};

        REQUIRE(DataManager::ValidateDataAlignment(trades, l2, on_status) == true);
        REQUIRE(last_status_msg == ""); // Es darf kein Fehler geloggt werden
    }

    SECTION("Geringe Lags (< 60 Min) am Anfang werden toleriert") {
        TradeEvent t1, t2;
        t1.timestamp = 0;
        t2.timestamp = 5000000;
        
        L2Snapshot l1, l2_snap;
        // L2 startet 59 Minuten später als die Trades
        l1.timestamp = 59 * MINUTE_MS; 
        l2_snap.timestamp = 5000000;

        std::vector<TradeEvent> trades = {t1, t2};
        std::vector<L2Snapshot> l2 = {l1, l2_snap};

        REQUIRE(DataManager::ValidateDataAlignment(trades, l2, on_status) == true);
    }

    SECTION("Asynchroner Start (> 60 Min) triggert Fehler") {
        TradeEvent t1, t2;
        t1.timestamp = 0;
        t2.timestamp = 5000000;
        
        L2Snapshot l1, l2_snap;
        // L2 startet 61 Minuten später als die Trades
        l1.timestamp = 61 * MINUTE_MS; 
        l2_snap.timestamp = 5000000;

        std::vector<TradeEvent> trades = {t1, t2};
        std::vector<L2Snapshot> l2 = {l1, l2_snap};

        REQUIRE(DataManager::ValidateDataAlignment(trades, l2, on_status) == false);
        REQUIRE_THAT(last_status_msg, Catch::Matchers::ContainsSubstring("ERR: DATA DESYNC START"));
    }

    SECTION("Asynchrones Ende (> 60 Min) triggert Fehler (z.B. Woche vs. Monat)") {
        TradeEvent t_start, t_end;
        t_start.timestamp = 0;
        t_end.timestamp = 30 * DAY_MS; // Trades laufen 30 Tage
        
        L2Snapshot l_start, l_end;
        l_start.timestamp = 0;
        l_end.timestamp = 7 * DAY_MS; // L2 läuft nur 7 Tage

        std::vector<TradeEvent> trades = {t_start, t_end};
        std::vector<L2Snapshot> l2 = {l_start, l_end};

        REQUIRE(DataManager::ValidateDataAlignment(trades, l2, on_status) == false);
        REQUIRE_THAT(last_status_msg, Catch::Matchers::ContainsSubstring("ERR: DATA DESYNC END"));
    }
}
