// Structural-exit tests: config resolution, rule behaviour, and the guard's
// close path. A self-contained MockBroker (no PaperTrader, no venue adapter)
// keeps the assertions on the DECISION, not the fill.

#include <catch2/catch_test_macros.hpp>

#include "config_parser.hpp"
#include "market_context.hpp"
#include "position_manager.hpp"
#include "structural_exit.hpp"
#include "trade_management.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using nlohmann::json;

namespace {

// Minimal IBroker: one open position, and a record of every ClosePartial
// call. A ClosePartial(1.0) books a TradeRecord and flattens, matching the
// paper broker's contract.
class MockBroker : public IBroker {
 public:
  bool         has_open = false;
  TradeSignal  current{};
  double       balance = 10000.0;
  double       open_qty = 1.0;

  // The real fill time, on the engine clock. 0 means "unknown", which is what
  // every broker defaulted to before GetPositionEntryTime existed -- tests
  // that leave it alone exercise the latch-on-first-tick fallback.
  int64_t entry_time = 0;

  struct CloseCall {
    double fraction;
    double price;
    int64_t ts;
    std::string reason;
  };
  std::vector<CloseCall> closes;
  std::vector<TradeRecord> history;

  void Open(SignalDirection dir, double entry, double stop, double tp,
            double qty = 1.0) {
    has_open = true;
    open_qty = qty;
    current.direction   = dir;
    current.entry_price = entry;
    current.stop_loss   = stop;
    current.take_profit = tp;
    current.volume      = qty;
  }

  double GetBalance() const override { return balance; }
  double GetEquity() const override { return balance; }
  bool   HasOpenPosition() const override { return has_open; }
  TradeSignal GetCurrentPosition() const override {
    return has_open ? current : TradeSignal{};
  }
  void UpdateStopLoss(double new_sl) override {
    if (has_open) current.stop_loss = new_sl;
  }
  int64_t GetPositionEntryTime() const override { return entry_time; }
  void ProcessSignal(TradeSignal signal, double, int64_t) override {
    has_open = true;
    current = signal;
  }
  void CheckRisk(double, int64_t) override {}
  void ClosePartial(double fraction, double price, int64_t ts,
                    const std::string& reason) override {
    closes.push_back({fraction, price, ts, reason});
    if (fraction >= 1.0) {
      TradeRecord r{};
      r.entry_time = 0;
      r.exit_time = ts;
      r.direction = current.direction;
      r.entry_price = current.entry_price;
      r.exit_price = price;
      r.net_profit = 0.0;
      r.exit_reason = reason;
      history.push_back(r);
      has_open = false;
      current = TradeSignal{};
    }
  }
  double GetNetProfit() const override { return 0.0; }
  std::vector<TradeRecord> GetTradeHistory() const override { return history; }
  int64_t GetLastTradeTime() const override { return 0; }
  size_t  GetTradeCount() const override { return history.size(); }
  double  GetLastClosedSize() const override { return open_qty; }
};

// Four bar/book containers the MarketContext borrows from. Held together so
// lifetime is obvious in each test case.
struct FakeContext {
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

} // namespace

// ---------------------------------------------------------------------------
// Load-time: resolver + build-time split
// ---------------------------------------------------------------------------

TEST_CASE("structural_exit: absent block leaves the config disabled") {
  json tm = json::object();
  const auto cfg = mokuren::ResolveStructuralExitConfig(tm);
  REQUIRE_FALSE(cfg.enabled);
  REQUIRE(cfg.rules.empty());
  REQUIRE(cfg.min_holdtime_ms == 0);
}

TEST_CASE("structural_exit: enabled requires min_holdtime_ms > 0") {
  json tm = json::object();
  tm["structural_exit"] = {
      {"enabled", true},
      {"rules", json::array({{{"name", "MfeGiveback"}, {"giveback_r", 0.5}}})},
  };
  // Missing min_holdtime_ms.
  REQUIRE_THROWS_AS(mokuren::ResolveStructuralExitConfig(tm),
                    std::runtime_error);

  // Zero is rejected, too.
  tm["structural_exit"]["min_holdtime_ms"] = 0;
  REQUIRE_THROWS(mokuren::ResolveStructuralExitConfig(tm));

  // Valid value loads.
  tm["structural_exit"]["min_holdtime_ms"] = 5000;
  const auto cfg = mokuren::ResolveStructuralExitConfig(tm);
  REQUIRE(cfg.enabled);
  REQUIRE(cfg.min_holdtime_ms == 5000);
  REQUIRE(cfg.rules.size() == 1);
  REQUIRE(cfg.rules[0].name == "MfeGiveback");
  REQUIRE(cfg.rules[0].enabled);
}

TEST_CASE("structural_exit: all rules disabled throws") {
  json tm = json::object();
  tm["structural_exit"] = {
      {"enabled", true},
      {"min_holdtime_ms", 5000},
      {"rules", json::array({
          {{"name", "MfeGiveback"}, {"giveback_r", 0.5}, {"enabled", false}},
          {{"name", "TimeStop"}, {"max_hold_ms", 1000}, {"active", "false"}},
      })},
  };
  REQUIRE_THROWS(mokuren::ResolveStructuralExitConfig(tm));
}

TEST_CASE("structural_exit: absent master switch means disabled, rules ignored") {
  json tm = json::object();
  tm["structural_exit"] = {
      // no "enabled" key at all
      {"rules", json::array({{{"name", "NoSuchRule"}}})},
  };
  // A disabled block does not validate anything, so an unknown rule name in a
  // disabled block is not an error (matches "a disabled rule's params are
  // never read").
  REQUIRE_NOTHROW(mokuren::ResolveStructuralExitConfig(tm));
  const auto cfg = mokuren::ResolveStructuralExitConfig(tm);
  REQUIRE_FALSE(cfg.enabled);
}

TEST_CASE("structural_exit: unknown rule name throws at build time") {
  mokuren::StructuralExitConfig cfg;
  cfg.enabled = true;
  cfg.min_holdtime_ms = 5000;
  cfg.rules.push_back({"NoSuchRule", true, json::object()});
  REQUIRE_THROWS(mokuren::BuildExitPlan(cfg));
}

// ---------------------------------------------------------------------------
// Rule behaviour
// ---------------------------------------------------------------------------

TEST_CASE("structural_exit: MfeGiveback fires after giveback_r of retrace") {
  mokuren::MfeGivebackRule giveback(0.5);

  TradeSignal pos{};
  pos.direction   = SignalDirection::BUY;
  pos.entry_price = 100.0;
  pos.stop_loss   = 98.0;  // 1R = 2.0

  FakeContext fc;
  mokuren::ExitState st;
  st.entry_ts_ms = 0;
  st.mfe = 4.0;  // 2R at the peak

  // Price at 103 (profit 3 = 1.5R): 1.5 <= 2.0 - 0.5 -> fire.
  auto sig = giveback.OnTrade(fc.ctx(), pos, 103.0, 1000, st);
  REQUIRE(sig.action == mokuren::ExitAction::Close);
  REQUIRE(std::string(sig.reason) == "MfeGiveback");

  // Price at 104 (profit 4 = 2R): 2.0 <= 1.5 -> false, hold.
  sig = giveback.OnTrade(fc.ctx(), pos, 104.0, 1000, st);
  REQUIRE(sig.action == mokuren::ExitAction::Hold);
}

TEST_CASE("structural_exit: MfeGiveback does not fire before earning giveback") {
  mokuren::MfeGivebackRule giveback(0.5);

  TradeSignal pos{};
  pos.direction   = SignalDirection::BUY;
  pos.entry_price = 100.0;
  pos.stop_loss   = 98.0;

  FakeContext fc;
  mokuren::ExitState st;
  st.entry_ts_ms = 0;
  st.mfe = 0.6;  // 0.3R at the peak -- less than the 0.5R giveback

  // Price at 99 (profit -1 = -0.5R). A naive rule would fire here; the
  // earned-giveback gate must suppress it.
  const auto sig = giveback.OnTrade(fc.ctx(), pos, 99.0, 1000, st);
  REQUIRE(sig.action == mokuren::ExitAction::Hold);
}

// ---------------------------------------------------------------------------
// Guard behaviour
// ---------------------------------------------------------------------------

TEST_CASE("structural_exit: guard enforces min_holdtime_ms") {
  MockBroker broker;
  broker.Open(SignalDirection::BUY, 100.0, 98.0, 105.0);

  mokuren::StructuralExitConfig cfg;
  cfg.enabled = true;
  cfg.min_holdtime_ms = 5000;
  cfg.rules.push_back({"MfeGiveback", true, json{{"giveback_r", 0.5}}});

  mokuren::StructuralExitGuard guard;
  guard.Configure(cfg);
  REQUIRE(guard.Enabled());

  FakeContext fc;

  // Prime mfe with a 2R favourable tick at t=1000.
  guard.OnTrade(fc.ctx(), &broker, 104.0, 1000);
  REQUIRE(broker.closes.empty());  // min_holdtime_ms = 5000; only 0 ms elapsed

  // Retrace to 1.5R at t=6000: 5000 ms elapsed -> giveback fires.
  guard.OnTrade(fc.ctx(), &broker, 103.0, 6000);
  REQUIRE(broker.closes.size() == 1);
  REQUIRE(broker.closes[0].reason == "struct_MfeGiveback");
  REQUIRE_FALSE(broker.HasOpenPosition());
}

TEST_CASE("structural_exit: disabled master switch leaves the guard inert") {
  MockBroker broker;
  broker.Open(SignalDirection::BUY, 100.0, 98.0, 105.0);

  mokuren::StructuralExitConfig cfg;
  cfg.enabled = false;
  cfg.min_holdtime_ms = 0;
  // A rule present in the config, but the master switch is off -- the guard
  // must not evaluate it.
  cfg.rules.push_back({"MfeGiveback", true, json{{"giveback_r", 0.5}}});

  mokuren::StructuralExitGuard guard;
  guard.Configure(cfg);
  REQUIRE_FALSE(guard.Enabled());

  FakeContext fc;
  guard.OnTrade(fc.ctx(), &broker, 103.0, 60000);
  REQUIRE(broker.closes.empty());
  REQUIRE(broker.HasOpenPosition());
}

TEST_CASE("structural_exit: only enabled rules fire") {
  MockBroker broker;
  broker.Open(SignalDirection::BUY, 100.0, 98.0, 105.0);

  mokuren::StructuralExitConfig cfg;
  cfg.enabled = true;
  cfg.min_holdtime_ms = 100;
  cfg.rules.push_back({"MfeGiveback", true,  json{{"giveback_r", 0.5}}});
  cfg.rules.push_back({"TimeStop",    false, json{{"max_hold_ms", 1}}});

  mokuren::StructuralExitGuard guard;
  guard.Configure(cfg);
  REQUIRE(guard.Enabled());

  FakeContext fc;
  // Two ticks after the hold gate opens: prime mfe at 2R, then give back to
  // 1.5R. The disabled TimeStop (max_hold_ms=1) WOULD fire on the second tick,
  // so firing only once proves the disabled rule really was skipped.
  guard.OnTrade(fc.ctx(), &broker, 104.0, 200);
  guard.OnTrade(fc.ctx(), &broker, 103.0, 300);

  REQUIRE(broker.closes.size() == 1);
  REQUIRE(broker.closes[0].reason == "struct_MfeGiveback");
}

TEST_CASE("structural_exit: UpdateOnL2 never fires BE/trail/scale") {
  MockBroker broker;
  broker.Open(SignalDirection::BUY, 100.0, 98.0, 105.0);

  // Structural disabled; only the BE/trail/scale ladder could fire.
  mokuren::StructuralExitConfig cfg;
  cfg.enabled = false;

  PositionManager pm(&broker, /*enable_scale=*/true, /*scale_fraction=*/0.7);
  pm.ConfigureStructuralExit(cfg);
  // BE never, trailing never, scale-out at 1R.
  pm.Configure(/*be_trigger_r=*/1e9,
               /*be_target_r=*/0.0,
               /*enable_trailing=*/false,
               /*trailing_trigger_r=*/0.0,
               /*trailing_dist_r=*/0.0,
               /*enable_scale_out=*/true,
               /*scale_out_trigger_r=*/1.0,
               /*scale_fraction=*/0.7);

  FakeContext fc;
  fc.latest_l2.best_bid_price = 105.0;   // profit = 5 = 2.5R -> would scale
  fc.latest_l2.best_ask_price = 105.1;

  // Book-path update must NOT trigger scale-out: BE/trail/scale stay on the
  // trade path.
  pm.UpdateOnL2(fc.ctx(), 1000);
  REQUIRE(broker.closes.empty());
  REQUIRE(broker.HasOpenPosition());

  // Sanity: the trade path with the same price DOES scale out. This is what
  // proves the assertion above is about the path, not a broken config.
  pm.Update(fc.ctx(), 105.0, 1000);
  REQUIRE(broker.closes.size() == 1);
  REQUIRE(broker.closes[0].reason == "scale_out");
}

TEST_CASE("structural_exit: ImbalanceBreak fires on a collapse from favourable") {
  mokuren::ImbalanceBreakRule rule(/*ratio=*/2.5, /*min_levels=*/1);

  TradeSignal pos{};
  pos.direction   = SignalDirection::BUY;
  pos.entry_price = 100.0;
  pos.stop_loss   = 98.0;

  FakeContext fc;
  mokuren::ExitState st;
  st.entry_ts_ms = 0;

  // Frame 1: bid 5, ask 1 -> ratio 5.0 >= 2.5 -> seen_above_ latches, Hold.
  fc.latest_l2.bids[0].price = 99.9;
  fc.latest_l2.bids[0].qty   = 5.0;
  fc.latest_l2.asks[0].price = 100.1;
  fc.latest_l2.asks[0].qty   = 1.0;
  auto sig = rule.OnBook(fc.ctx(), pos, 1000, st);
  REQUIRE(sig.action == mokuren::ExitAction::Hold);

  // Frame 2: qty flips -> ratio 1/5 = 0.2 < 2.5, and seen_above_ -> fire.
  fc.latest_l2.bids[0].qty = 1.0;
  fc.latest_l2.asks[0].qty = 5.0;
  sig = rule.OnBook(fc.ctx(), pos, 2000, st);
  REQUIRE(sig.action == mokuren::ExitAction::Close);
  REQUIRE(std::string(sig.reason) == "ImbalanceBreak");
}

TEST_CASE("structural_exit: DeltaFlip fires only on a flip against the position") {
  mokuren::DeltaFlipRule rule(/*min_delta=*/50.0);

  TradeSignal pos{};
  pos.direction   = SignalDirection::BUY;
  pos.entry_price = 100.0;
  pos.stop_loss   = 98.0;

  FakeContext fc;
  mokuren::ExitState st;
  st.entry_ts_ms = 0;

  // Long, delta -60: past the -50 threshold -> close.
  fc.live_bar.cumulative_delta = -60.0;
  auto sig = rule.OnTrade(fc.ctx(), pos, 99.0, 1000, st);
  REQUIRE(sig.action == mokuren::ExitAction::Close);
  REQUIRE(std::string(sig.reason) == "DeltaFlip");

  // Long, delta -10: against the position, but short of the threshold.
  fc.live_bar.cumulative_delta = -10.0;
  REQUIRE(rule.OnTrade(fc.ctx(), pos, 99.0, 1000, st).action ==
          mokuren::ExitAction::Hold);

  // Long, delta +80: strong, but WITH the position -> hold.
  fc.live_bar.cumulative_delta = 80.0;
  REQUIRE(rule.OnTrade(fc.ctx(), pos, 101.0, 1000, st).action ==
          mokuren::ExitAction::Hold);

  // The short side mirrors: +60 closes, -80 holds.
  pos.direction = SignalDirection::SELL;
  fc.live_bar.cumulative_delta = 60.0;
  REQUIRE(rule.OnTrade(fc.ctx(), pos, 101.0, 1000, st).action ==
          mokuren::ExitAction::Close);

  fc.live_bar.cumulative_delta = -80.0;
  REQUIRE(rule.OnTrade(fc.ctx(), pos, 99.0, 1000, st).action ==
          mokuren::ExitAction::Hold);
}

TEST_CASE("structural_exit: DeltaDivergence fires on price/delta disagreement") {
  mokuren::DeltaDivergenceRule rule(/*lookback=*/2);

  TradeSignal pos{};
  pos.direction   = SignalDirection::BUY;
  pos.entry_price = 100.0;
  pos.stop_loss   = 98.0;

  FakeContext fc;
  mokuren::ExitState st;
  st.entry_ts_ms = 0;

  // lookback=2 compares hist[size-2]; two closed bars is the minimum.
  Bar closed{};
  closed.close = 100.0;
  closed.cumulative_delta = 50.0;
  fc.history.push_back(closed);
  closed.close = 100.5;
  closed.cumulative_delta = 30.0;
  fc.history.push_back(closed);

  // Long, price up to 101 but delta down to 10 -> divergence.
  fc.live_bar.close = 101.0;
  fc.live_bar.cumulative_delta = 10.0;
  auto sig = rule.OnTrade(fc.ctx(), pos, 101.0, 1000, st);
  REQUIRE(sig.action == mokuren::ExitAction::Close);
  REQUIRE(std::string(sig.reason) == "DeltaDivergence");

  // Long, price up AND delta up -> agreement, hold.
  fc.live_bar.cumulative_delta = 70.0;
  REQUIRE(rule.OnTrade(fc.ctx(), pos, 101.0, 1000, st).action ==
          mokuren::ExitAction::Hold);

  // Short, price down to 99 but delta up -> divergence.
  pos.direction = SignalDirection::SELL;
  fc.live_bar.close = 99.0;
  fc.live_bar.cumulative_delta = 80.0;
  REQUIRE(rule.OnTrade(fc.ctx(), pos, 99.0, 1000, st).action ==
          mokuren::ExitAction::Close);
}

TEST_CASE("structural_exit: SpoofPull fires when the leaning level is pulled") {
  mokuren::SpoofPullRule rule(/*drop_pct=*/0.5, /*min_qty=*/5.0);

  TradeSignal pos{};
  pos.direction   = SignalDirection::BUY;
  pos.entry_price = 100.0;
  pos.stop_loss   = 98.0;

  // One resting bid of bid_qty at 99.9, with a token ask so the frame is not
  // half-empty.
  auto frame = [](double bid_qty) {
    L2Snapshot s{};
    s.bids[0].price = 99.9;
    s.bids[0].qty   = bid_qty;
    s.asks[0].price = 100.1;
    s.asks[0].qty   = 1.0;
    return s;
  };

  FakeContext fc;
  mokuren::ExitState st;
  st.entry_ts_ms = 0;

  // A single frame is not a comparison: the rule needs a previous one.
  fc.l2_history.push(frame(10.0));
  fc.latest_l2 = frame(10.0);
  REQUIRE(rule.OnBook(fc.ctx(), pos, 1000, st).action ==
          mokuren::ExitAction::Hold);

  // 10 -> 8 is a 20% drop, below the 50% threshold.
  fc.l2_history.push(frame(8.0));
  fc.latest_l2 = frame(8.0);
  REQUIRE(rule.OnBook(fc.ctx(), pos, 2000, st).action ==
          mokuren::ExitAction::Hold);

  // 8 -> 4 is exactly 50% off the PREVIOUS frame, not off the first one.
  fc.l2_history.push(frame(4.0));
  fc.latest_l2 = frame(4.0);
  auto sig = rule.OnBook(fc.ctx(), pos, 3000, st);
  REQUIRE(sig.action == mokuren::ExitAction::Close);
  REQUIRE(std::string(sig.reason) == "SpoofPull");

  // A level that vanishes entirely is a 100% pull, which the >= min_qty
  // gate must still count as a pull rather than skip.
  FakeContext gone;
  gone.l2_history.push(frame(10.0));
  gone.latest_l2 = frame(10.0);
  L2Snapshot no_bid{};
  no_bid.asks[0].price = 100.1;
  no_bid.asks[0].qty   = 1.0;
  gone.l2_history.push(no_bid);
  gone.latest_l2 = no_bid;
  REQUIRE(rule.OnBook(gone.ctx(), pos, 4000, st).action ==
          mokuren::ExitAction::Close);
}

TEST_CASE("structural_exit: TimeStop closes once the trade outlives max_hold_ms") {
  MockBroker broker;
  broker.Open(SignalDirection::BUY, 100.0, 98.0, 105.0);

  mokuren::StructuralExitConfig cfg;
  cfg.enabled = true;
  cfg.min_holdtime_ms = 1000;
  cfg.rules.push_back({"TimeStop", true, json{{"max_hold_ms", 5000}}});

  mokuren::StructuralExitGuard guard;
  guard.Configure(cfg);
  REQUIRE(guard.Enabled());

  FakeContext fc;

  // t=1000: the clock latches here (MockBroker reports no fill time), so
  // min_holdtime_ms is not yet satisfied even though TimeStop is.
  guard.OnTrade(fc.ctx(), &broker, 100.0, 1000);
  REQUIRE(broker.closes.empty());

  // t=2000: hold gate open, 1000 ms of trade -> still short of 5000.
  guard.OnTrade(fc.ctx(), &broker, 100.0, 2000);
  REQUIRE(broker.closes.empty());

  // t=6000: 5000 ms since the latch -> close.
  guard.OnTrade(fc.ctx(), &broker, 100.0, 6000);
  REQUIRE(broker.closes.size() == 1);
  REQUIRE(broker.closes[0].reason == "struct_TimeStop");
  REQUIRE_FALSE(broker.HasOpenPosition());
}

TEST_CASE("structural_exit: the trade clock comes from the broker when it knows it") {
  MockBroker broker;
  broker.Open(SignalDirection::BUY, 100.0, 98.0, 105.0);
  broker.entry_time = 1000;  // filled long before this process started

  mokuren::StructuralExitConfig cfg;
  cfg.enabled = true;
  cfg.min_holdtime_ms = 100;
  cfg.rules.push_back({"TimeStop", true, json{{"max_hold_ms", 5000}}});

  mokuren::StructuralExitGuard guard;
  guard.Configure(cfg);

  // First tick of the process, 60 s after the real fill: min_holdtime_ms is
  // long satisfied and the trade is already past max_hold_ms. Latching to
  // now_ms instead would restart the clock and keep a dead trade open for
  // another 5 s -- the restart case ExitState::entry_ts_ms exists for.
  FakeContext fc;
  guard.OnTrade(fc.ctx(), &broker, 100.0, 61000);
  REQUIRE(broker.closes.size() == 1);
  REQUIRE(broker.closes[0].reason == "struct_TimeStop");
}

TEST_CASE("structural_exit: a book close quotes the touch a market order crosses") {
  MockBroker broker;
  broker.Open(SignalDirection::BUY, 100.0, 98.0, 105.0);

  mokuren::StructuralExitConfig cfg;
  cfg.enabled = true;
  cfg.min_holdtime_ms = 1000;
  cfg.rules.push_back(
      {"ImbalanceBreak", true, json{{"ratio", 2.5}, {"min_levels", 1}}});

  mokuren::StructuralExitGuard guard;
  guard.Configure(cfg);

  FakeContext fc;
  fc.latest_l2.bids[0].price  = 99.9;
  fc.latest_l2.bids[0].qty    = 5.0;
  fc.latest_l2.asks[0].price  = 100.1;
  fc.latest_l2.asks[0].qty    = 1.0;
  fc.latest_l2.best_bid_price = 99.9;
  fc.latest_l2.best_ask_price = 100.1;

  // A print has to establish the clock first: OnBook deliberately refuses to
  // latch one from a broker that answers 0, so the book path alone is inert
  // on a cold start.
  guard.OnTrade(fc.ctx(), &broker, 100.0, 1000);
  REQUIRE(broker.closes.empty());

  // t=2000: hold gate open, favourable imbalance latches the rule.
  guard.OnBook(fc.ctx(), &broker, 2000);
  REQUIRE(broker.closes.empty());

  // t=3000: the imbalance collapses -> close at the BID, the side a long
  // actually crosses. Booking the mid (100.0) would credit half a spread on
  // every book close, in one direction.
  fc.latest_l2.bids[0].qty = 1.0;
  fc.latest_l2.asks[0].qty = 5.0;
  guard.OnBook(fc.ctx(), &broker, 3000);
  REQUIRE(broker.closes.size() == 1);
  REQUIRE(broker.closes[0].reason == "struct_ImbalanceBreak");
  REQUIRE(broker.closes[0].price == 99.9);
}

TEST_CASE("structural_exit: a new trade starts with clean excursion state") {
  MockBroker broker;
  mokuren::StructuralExitConfig cfg;
  cfg.enabled = true;
  cfg.min_holdtime_ms = 100;
  cfg.rules.push_back({"MfeGiveback", true, json{{"giveback_r", 0.5}}});

  // Structural only: SetEnabled(false) silences BE/trail/scale but leaves the
  // guard armed, so nothing else can book a close here.
  PositionManager pm(&broker, /*enable_scale=*/false, /*scale_fraction=*/0.0);
  pm.SetEnabled(false);
  pm.ConfigureStructuralExit(cfg);

  FakeContext fc;

  // Trade 1: run to 2R, then give back to 1.5R -> close.
  broker.Open(SignalDirection::BUY, 100.0, 98.0, 105.0);
  pm.Update(fc.ctx(), 104.0, 100);
  pm.Update(fc.ctx(), 103.0, 1000);
  REQUIRE(broker.closes.size() == 1);
  REQUIRE(broker.closes[0].reason == "struct_MfeGiveback");
  REQUIRE_FALSE(broker.HasOpenPosition());

  // Flat tick: this is where PositionManager resets the guard's state.
  pm.Update(fc.ctx(), 103.0, 1100);

  // Trade 2 opens at the same price and drifts to 0.5R with no giveback
  // earned. Carrying mfe = 4 (2R) from trade 1 would make MfeGiveback fire
  // on the first evaluated tick -- a hidden stop loss on a trade that never
  // moved.
  broker.Open(SignalDirection::BUY, 100.0, 98.0, 105.0);
  pm.Update(fc.ctx(), 101.0, 1200);   // latches the trade clock
  pm.Update(fc.ctx(), 101.0, 1300);   // hold gate open; mfe_r == profit_r
  REQUIRE(broker.closes.size() == 1);  // still trade 1's close
  REQUIRE(broker.HasOpenPosition());
}

TEST_CASE("structural_exit: a new fill identity resets state with no flat tick") {
  MockBroker broker;
  mokuren::StructuralExitConfig cfg;
  cfg.enabled = true;
  cfg.min_holdtime_ms = 100;
  cfg.rules.push_back({"MfeGiveback", true, json{{"giveback_r", 0.5}}});

  mokuren::StructuralExitGuard guard;
  guard.Configure(cfg);

  FakeContext fc;

  // Trade 1: fill at t=1000, run to 2R (mfe = 4 = 2R).
  broker.Open(SignalDirection::BUY, 100.0, 98.0, 105.0);
  broker.entry_time = 1000;
  guard.OnTrade(fc.ctx(), &broker, 104.0, 2000);
  REQUIRE(broker.closes.empty());

  // Trade 2 replaces it in place: same prices, a NEW fill time, and no flat
  // tick in between, so PositionManager's reset never runs. At 101 the trade
  // is +0.5R with nothing given back; an mfe of 2R surviving from trade 1
  // would fire MfeGiveback here.
  broker.Open(SignalDirection::BUY, 100.0, 98.0, 105.0);
  broker.entry_time = 5000;
  guard.OnTrade(fc.ctx(), &broker, 101.0, 5100);
  REQUIRE(broker.closes.empty());
  REQUIRE(broker.HasOpenPosition());
}

TEST_CASE("structural_exit: without a fill time two trades look alike") {
  MockBroker broker;  // entry_time stays 0: no identity to compare
  mokuren::StructuralExitConfig cfg;
  cfg.enabled = true;
  cfg.min_holdtime_ms = 100;
  cfg.rules.push_back({"MfeGiveback", true, json{{"giveback_r", 0.5}}});

  mokuren::StructuralExitGuard guard;
  guard.Configure(cfg);

  FakeContext fc;

  // Trade 1: the clock latches to the first tick seen (t=2000), mfe to 2R.
  broker.Open(SignalDirection::BUY, 100.0, 98.0, 105.0);
  guard.OnTrade(fc.ctx(), &broker, 104.0, 2000);
  REQUIRE(broker.closes.empty());

  // Trade 2 in the same call sequence. The guard cannot tell it from trade 1,
  // so the inherited mfe fires MfeGiveback on a +0.5R tick. This pins the
  // documented limitation: resetting here is PositionManager's job on a
  // broker that cannot answer GetPositionEntryTime.
  broker.Open(SignalDirection::BUY, 100.0, 98.0, 105.0);
  guard.OnTrade(fc.ctx(), &broker, 101.0, 5100);
  REQUIRE(broker.closes.size() == 1);
  REQUIRE(broker.closes[0].reason == "struct_MfeGiveback");
}
