// Parity: the structural guard driven through the real PaperTrader.
//
// test_structural_exit.cpp asserts the DECISION against a MockBroker that
// applies no fees and no slippage. That is the right stand-in for "did the rule
// fire" and the wrong one for "what did the exit cost": every price the guard
// hands to IBroker::ClosePartial is re-priced by the broker, and only
// PaperTrader does that re-pricing. This file runs the same rule through
// PaperTrader so the reason string, the exit price, the two fees and the
// balance movement are checked against a hand-computed fill.

#include <catch2/catch_test_macros.hpp>

#include "aggregator.hpp"
#include "market_context.hpp"
#include "papertrader.hpp"
#include "position_manager.hpp"
#include "structural_exit.hpp"
#include "trade_management.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using nlohmann::json;

namespace {

// Same shape as the fixture in test_structural_exit.cpp: the four containers
// MarketContext borrows, held together so their lifetime is obvious.
struct Book {
  std::vector<Bar> history;
  Bar              live_bar;
  Bar              htf_bar;
  SessionMetrics   session{};
  L2Snapshot       latest_l2{};
  L2RingBuffer     l2_history{};

  MarketContext ctx() {
    return MarketContext{history, live_bar, htf_bar, session, latest_l2,
                         l2_history};
  }
};

constexpr double kSlippage = 0.0002;
constexpr double kTakerFee = 0.0004;

// A long entry the paper broker accepts: entry 100, stop 98 (1R = 2%), target
// far enough away that it cannot interfere with a structural close.
TradeSignal LongEntry() {
  TradeSignal sig{};
  sig.direction   = SignalDirection::BUY;
  sig.entry_price = 100.0;
  sig.stop_loss   = 98.0;
  sig.take_profit = 105.0;
  sig.volume      = 1.0;
  return sig;
}

// PositionManager with the BE/trail/scale ladder out of the way, so the only
// thing that can close the trade is what the test is about.
void DisableLadder(PositionManager& pm) {
  pm.Configure(/*be_trigger_r=*/1e9,
               /*be_target_r=*/0.0,
               /*enable_trailing=*/false,
               /*trailing_trigger_r=*/0.0,
               /*trailing_dist_r=*/0.0,
               /*enable_scale_out=*/false,
               /*scale_out_trigger_r=*/0.0,
               /*scale_fraction=*/0.0);
}

} // namespace

TEST_CASE("structural_exit parity: PaperTrader books a struct_ close",
          "[structural_exit][parity]") {
  PaperTrader broker(kSlippage, kTakerFee, 10000.0);
  // The ladder belongs to PositionManager; the broker's own copy would move
  // the same stop twice. Same call the factory makes on every path.
  broker.DisableManagement();

  PositionManager pm(&broker, /*enable_scale=*/false, /*scale_fraction=*/0.0);
  DisableLadder(pm);

  mokuren::StructuralExitConfig cfg;
  cfg.enabled = true;
  cfg.min_holdtime_ms = 1000;
  cfg.rules.push_back({"MfeGiveback", true, json{{"giveback_r", 0.5}}});
  pm.ConfigureStructuralExit(cfg);

  Book book;
  broker.ProcessSignal(LongEntry(), 100.0, 1000);
  REQUIRE(broker.HasOpenPosition());

  // The broker slips the entry, so the 1R the guard measures is the placed
  // stop distance, not the 2.0 the signal asked for.
  const double entry_price = broker.GetCurrentPosition().entry_price;
  REQUIRE(std::abs(entry_price - 100.0 * (1.0 + kSlippage)) < 1e-9);
  const double one_r =
      std::abs(entry_price - broker.GetCurrentPosition().stop_loss);
  REQUIRE(one_r > 0.0);

  // t=1000: clock comes from the broker (entry at 1000); 0 ms elapsed, so the
  // hold gate blocks everything and mfe stays 0.
  pm.Update(book.ctx(), 100.0, 1000);
  REQUIRE(broker.HasOpenPosition());

  // t=2000: +1.49R of excursion -- above giveback_r, but nothing given back.
  pm.Update(book.ctx(), 103.0, 2000);
  REQUIRE(broker.HasOpenPosition());

  // t=3000: back to +0.94R, i.e. 0.55R off the peak -> fires.
  pm.Update(book.ctx(), 101.9, 3000);
  REQUIRE_FALSE(broker.HasOpenPosition());

  const auto hist = broker.GetTradeHistory();
  REQUIRE(hist.size() == 1);
  const auto& rec = hist[0];
  REQUIRE(rec.exit_reason == "struct_MfeGiveback");
  REQUIRE(rec.exit_time == 3000);

  // The exit price is the price the ENGINE passed, slipped by the broker --
  // the guard does not price the fill, it only decides.
  const double expected_exit = 101.9 * (1.0 - kSlippage);
  REQUIRE(std::abs(rec.exit_price - expected_exit) < 1e-9);

  // Same arithmetic PaperTrader::ClosePartial does, written out here so the
  // test fails if that close path changes underneath the guard.
  const double gross     = (expected_exit - entry_price) * 1.0;
  const double exit_fee  = expected_exit * 1.0 * kTakerFee;
  const double entry_fee = entry_price * 1.0 * kTakerFee;
  REQUIRE(std::abs(rec.net_profit - (gross - exit_fee - entry_fee)) < 1e-9);

  // Balance moved by the exit only: the entry fee was taken at fill time.
  REQUIRE(std::abs(broker.GetBalance() -
                   (10000.0 - entry_fee + gross - exit_fee)) < 1e-9);
}

TEST_CASE("structural_exit parity: disabled block leaves the trade open",
          "[structural_exit][parity]") {
  PaperTrader broker(kSlippage, kTakerFee, 10000.0);
  broker.DisableManagement();

  PositionManager pm(&broker, /*enable_scale=*/false, /*scale_fraction=*/0.0);
  DisableLadder(pm);

  // Same config, master switch off. This is the A/B the backtest config
  // promises: no structural exit exists, so the identical sequence produces no
  // record and no struct_ reason.
  mokuren::StructuralExitConfig cfg;
  cfg.enabled = false;
  cfg.min_holdtime_ms = 1000;
  cfg.rules.push_back({"MfeGiveback", true, json{{"giveback_r", 0.5}}});
  pm.ConfigureStructuralExit(cfg);

  Book book;
  broker.ProcessSignal(LongEntry(), 100.0, 1000);

  pm.Update(book.ctx(), 100.0, 1000);
  pm.Update(book.ctx(), 103.0, 2000);
  pm.Update(book.ctx(), 101.9, 3000);

  REQUIRE(broker.HasOpenPosition());
  REQUIRE(broker.GetTradeHistory().empty());
}

TEST_CASE("structural_exit parity: the trade clock comes from the broker",
          "[structural_exit][parity][restart]") {
  PaperTrader broker(kSlippage, kTakerFee, 10000.0);
  broker.DisableManagement();

  PositionManager pm(&broker, /*enable_scale=*/false, /*scale_fraction=*/0.0);
  DisableLadder(pm);

  mokuren::StructuralExitConfig cfg;
  cfg.enabled = true;
  cfg.min_holdtime_ms = 30000;
  cfg.rules.push_back({"TimeStop", true, json{{"max_hold_ms", 60000}}});
  pm.ConfigureStructuralExit(cfg);

  Book book;
  broker.ProcessSignal(LongEntry(), 100.0, 1000);  // filled at t=1000
  REQUIRE(broker.GetPositionEntryTime() == 1000);

  // The first tick this guard ever sees is 99 s later -- a restart with the
  // position already open. Because the broker reports the real fill time, both
  // the 30 s hold gate and the 60 s max hold are already served, so the trade
  // closes on this very tick. Latching to the restart instead would have
  // waited another 60 s.
  pm.Update(book.ctx(), 100.5, 100000);

  REQUIRE_FALSE(broker.HasOpenPosition());
  const auto hist = broker.GetTradeHistory();
  REQUIRE(hist.size() == 1);
  REQUIRE(hist[0].exit_reason == "struct_TimeStop");
}
