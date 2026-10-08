#include <catch2/catch_test_macros.hpp>
#include "trigger.hpp"
#include "market_context.hpp"

TEST_CASE("trigger verification suite", "[triggers]") {
    // setup empty baseline state
    std::vector<Bar> mock_history;
    Bar mock_live, mock_htf;
    SessionMetrics mock_session;
    L2Snapshot mock_l2{}; // 0-initialisierung für die neuen c-arrays
    L2RingBuffer mock_l2_history;

    SECTION("spoofhunter detects pulled l2 limit walls") {
        SpoofHunterTrigger hunter(500, 30.0, 0.9);

        // t=0: massive fake ask wall appears
        L2Snapshot snap1{};
        snap1.timestamp = 1000;
        snap1.asks[0].qty = 50.0; // NEU: nutzt das aggregations-array
        mock_l2_history.push(snap1);

        // t=250: wall still active
        L2Snapshot snap2{};
        snap2.timestamp = 1250;
        snap2.asks[0].qty = 50.0;
        mock_l2_history.push(snap2);

        // t=500: wall suddenly pulled (drops to 1.0 btc)
        L2Snapshot current_l2{};
        current_l2.timestamp = 1500;
        current_l2.asks[0].qty = 1.0; 

        MarketContext ctx{mock_history, mock_live, mock_htf, mock_session, current_l2, mock_l2_history};

        TradeSignal sig = hunter.EvaluateTick(ctx);
        REQUIRE(sig.direction == SignalDirection::BUY);

        // t=500: test bid spoofing (drops to 2.0 btc)
        L2Snapshot current_l2_bid{};
        current_l2_bid.timestamp = 1500;
        current_l2_bid.bids[0].qty = 2.0; // NEU: nutzt das aggregations-array

        snap1.asks[0].qty = 0.0;
        snap1.bids[0].qty = 40.0;
        mock_l2_history.push(snap1); // overwrite history for bid test

        MarketContext ctx_bid{mock_history, mock_live, mock_htf, mock_session, current_l2_bid, mock_l2_history};
        
        TradeSignal sig_bid = hunter.EvaluateTick(ctx_bid);
        REQUIRE(sig_bid.direction == SignalDirection::SELL);
    }

    SECTION("stackedimbalance detects footprint ratio chains") {
        StackedImbalanceTrigger trigger(3.0, 2); 
        
        Bar bar;
        PriceLevel l0, l1, l2; 
        
        // setup buy imbalance (upper ask is 3x larger than lower bid)
        l0.price = 100.0; l0.bid_volume = 2.0; l0.ask_volume = 0.0;
        l1.price = 101.0; l1.bid_volume = 2.0; l1.ask_volume = 10.0; // imb 1
        l2.price = 102.0; l2.bid_volume = 1.0; l2.ask_volume = 15.0; // imb 2
        
        bar.footprint = {l0, l1, l2};
        mock_history.push_back(bar);
        
        MarketContext ctx{mock_history, mock_live, mock_htf, mock_session, mock_l2, mock_l2_history};
        
        TradeSignal sig = trigger.EvaluateCandle(ctx);
        REQUIRE(sig.direction == SignalDirection::BUY);

        // setup sell imbalance (upper bid is 3x larger than lower ask)
        l0.price = 100.0; l0.bid_volume = 0.0; l0.ask_volume = 2.0;
        l1.price = 101.0; l1.bid_volume = 10.0; l1.ask_volume = 2.0; // imb 1
        l2.price = 102.0; l2.bid_volume = 15.0; l2.ask_volume = 1.0; // imb 2
        
        bar.footprint = {l0, l1, l2};
        mock_history[0] = bar; // replace history
        
        TradeSignal sig2 = trigger.EvaluateCandle(ctx);
        REQUIRE(sig2.direction == SignalDirection::SELL);
    }

    SECTION("deltaabsorption catches counter-trend heavy flow") {
        DeltaAbsorptionTrigger trigger(50.0);
        
        Bar bar;
        bar.tick_size = 1.0;
        
        // buy setup: heavy negative delta but price closes green
        bar.open = 100.0;
        bar.close = 105.0; 
        bar.low = 98.0;
        
        PriceLevel l0; 
        l0.price = 100.0;
        l0.ask_volume = 10.0;
        l0.bid_volume = 80.0; // net delta = -70
        bar.footprint = {l0};
        
        mock_history.push_back(bar);
        MarketContext ctx{mock_history, mock_live, mock_htf, mock_session, mock_l2, mock_l2_history};
        
        TradeSignal sig = trigger.EvaluateCandle(ctx);
        REQUIRE(sig.direction == SignalDirection::BUY);
        REQUIRE(sig.entry_price == 105.0);
        REQUIRE(sig.stop_loss == 96.0); // 98.0 - (1.0 * 2)
        
        // sell setup: heavy positive delta but price closes red
        bar.open = 105.0;
        bar.close = 100.0;
        
        l0.ask_volume = 80.0;
        l0.bid_volume = 10.0; // net delta = +70
        bar.footprint = {l0};
        
        mock_history[0] = bar;
        
        TradeSignal sig2 = trigger.EvaluateCandle(ctx);
        REQUIRE(sig2.direction == SignalDirection::SELL);
    }

    SECTION("logical blocks route signals correctly") {
        DeltaAbsorptionTrigger buy_trigger(50.0);
        StackedImbalanceTrigger sell_trigger(3.0, 2);

        OR_Trigger or_block;
        or_block.AddTrigger(&buy_trigger);
        or_block.AddTrigger(&sell_trigger);

        // mock a delta absorption buy setup
        Bar bar;
        bar.tick_size = 1.0;
        bar.open = 100.0; bar.close = 105.0; bar.low = 98.0;
        PriceLevel l0; 
        l0.price = 100.0; l0.ask_volume = 10.0; l0.bid_volume = 80.0; 
        bar.footprint = {l0};
        mock_history.push_back(bar);
        
        MarketContext ctx{mock_history, mock_live, mock_htf, mock_session, mock_l2, mock_l2_history};

        // or_block should return buy because delta absorption fires
        TradeSignal or_sig = or_block.EvaluateCandle(ctx);
        REQUIRE(or_sig.direction == SignalDirection::BUY);

        AND_Trigger and_block;
        and_block.AddTrigger(&buy_trigger);
        and_block.AddTrigger(&sell_trigger);

        // and_block should return none because stacked imbalance does NOT fire
        TradeSignal and_sig = and_block.EvaluateCandle(ctx);
        REQUIRE(and_sig.direction == SignalDirection::NONE);
    }
}