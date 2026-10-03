#include <catch2/catch_test_macros.hpp>
#include "filter.hpp"
#include "market_context.hpp"

TEST_CASE("Filter Verification Suite", "[filters]") {
    
    // 1. UNSERE KONTROLL-DATEN (normal und veränderbar)
    std::vector<Bar> mock_history;
    Bar mock_live;
    Bar mock_htf;
    SessionMetrics mock_session;
    L2Snapshot mock_l2;
    L2RingBuffer mock_l2_history; // Unser neuer Dummy-Puffer für die Tests

    TradeSignal buy_signal{SignalDirection::BUY};
    TradeSignal sell_signal{SignalDirection::SELL};

    SECTION("MinVolumeFilter blocks on low volume") {
        MinVolumeFilter filter(100.0); 
        
        Bar b1; b1.total_volume = 50.0;
        mock_history.push_back(b1);
        
        // 2. CONTEXT ERSCHAFFEN (Jetzt mit allen 6 Parametern!)
        MarketContext ctx{mock_history, mock_live, mock_htf, mock_session, mock_l2, mock_l2_history}; 
        
        REQUIRE(filter.AllowTrade(ctx, buy_signal) == false);

        mock_history.back().total_volume = 150.0;
        REQUIRE(filter.AllowTrade(ctx, buy_signal) == true);  
    }

    SECTION("POCTrendFilter respects point of control") {
        POCTrendFilter filter;
        
        Bar b1; b1.poc_price = 50000.0; b1.close = 50100.0;
        mock_history.push_back(b1);

        MarketContext ctx{mock_history, mock_live, mock_htf, mock_session, mock_l2, mock_l2_history};

        REQUIRE(filter.AllowTrade(ctx, buy_signal) == true);   
        REQUIRE(filter.AllowTrade(ctx, sell_signal) == false); 
        
        mock_history.back().close = 49900.0; 
        REQUIRE(filter.AllowTrade(ctx, buy_signal) == false);  
        REQUIRE(filter.AllowTrade(ctx, sell_signal) == true);  
    }

    SECTION("OrderbookImbalanceFilter checks L2 walls") {
        OrderbookImbalanceFilter filter(3.0); 
        MarketContext ctx{mock_history, mock_live, mock_htf, mock_session, mock_l2, mock_l2_history};

        ctx.latest_l2.best_bid_qty = 30.0;
        ctx.latest_l2.best_ask_qty = 10.0; 

        REQUIRE(filter.AllowTrade(ctx, buy_signal) == true);   
        REQUIRE(filter.AllowTrade(ctx, sell_signal) == false); 

        ctx.latest_l2.best_bid_qty = 20.0; 
        REQUIRE(filter.AllowTrade(ctx, buy_signal) == false);  
    }

    SECTION("VwapTrendFilter aligns with session vwap") {
        VwapTrendFilter filter(true); 
        MarketContext ctx{mock_history, mock_live, mock_htf, mock_session, mock_l2, mock_l2_history};
        
        ctx.session.vwap = 60000.0;
        mock_live.close = 60500.0; 

        REQUIRE(filter.AllowTrade(ctx, buy_signal) == true);   
        REQUIRE(filter.AllowTrade(ctx, sell_signal) == false); 
    }

    SECTION("MacroTrendFilter blocks against SMA") {
        MacroTrendFilter filter(3); 
        
        for (int i = 0; i < 3; ++i) {
            Bar b; b.close = 100.0;
            mock_history.push_back(b);
        }
        
        mock_history.back().close = 90.0; 
        MarketContext ctx{mock_history, mock_live, mock_htf, mock_session, mock_l2, mock_l2_history};
        
        REQUIRE(filter.AllowTrade(ctx, buy_signal) == false); 
        REQUIRE(filter.AllowTrade(ctx, sell_signal) == true); 
    }

    SECTION("CVDDivergenceFilter detects toxic flow") {
        CVDDivergenceFilter filter(2); 
        
        Bar b0; b0.close = 100.0; b0.cumulative_delta = 0.0;
        Bar b1; b1.close = 105.0; b1.cumulative_delta = -10.0;
        Bar b2; b2.close = 110.0; b2.cumulative_delta = -20.0;
        
        mock_history.push_back(b0);
        mock_history.push_back(b1);
        mock_history.push_back(b2);

        MarketContext ctx{mock_history, mock_live, mock_htf, mock_session, mock_l2, mock_l2_history};

        REQUIRE(filter.AllowTrade(ctx, buy_signal) == false); 
    }

    SECTION("VolatilityFilter checks minimum range") {
        VolatilityFilter filter(50.0, 2); 
        
        Bar b1; b1.high = 60100.0; b1.low = 60000.0; 
        Bar b2; b2.high = 60020.0; b2.low = 60000.0; 
        
        mock_history.push_back(b1);
        mock_history.push_back(b2);

        MarketContext ctx{mock_history, mock_live, mock_htf, mock_session, mock_l2, mock_l2_history};

        REQUIRE(filter.AllowTrade(ctx, buy_signal) == true); 

        mock_history.back().high = 60000.0; 
        REQUIRE(filter.AllowTrade(ctx, buy_signal) == true); 

        mock_history.back().high = 59900.0; 
        REQUIRE(filter.AllowTrade(ctx, buy_signal) == false); 
    }
    
    SECTION("RVOLFilter checks relative volume") {
        RVOLFilter filter(1.5, 2); 
        
        Bar b1; b1.total_volume = 100.0; 
        Bar b2; b2.total_volume = 100.0; 
        Bar b3; b3.total_volume = 150.0; 
        
        mock_history.push_back(b1);
        mock_history.push_back(b2);
        mock_history.push_back(b3);

        MarketContext ctx{mock_history, mock_live, mock_htf, mock_session, mock_l2, mock_l2_history};

        REQUIRE(filter.AllowTrade(ctx, buy_signal) == true); 

        mock_history.back().total_volume = 140.0; 
        REQUIRE(filter.AllowTrade(ctx, buy_signal) == false);
    }
}