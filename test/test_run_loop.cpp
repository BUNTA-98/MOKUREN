// Regression tests for the shared backtest loop.
//
// RunLoop is the single implementation of "walk the prints, catch the book up,
// evaluate, execute" that five drivers used to duplicate. Four of those copies
// were missing at least one of the three behaviours pinned here, and none of
// the three is visible from a completed run: a truncated book tail looks like a
// quiet market, a forced close at 0.0 looks like a catastrophic but plausible
// strategy, and an early exit timestamp only moves a marker in the inspector.
// That is why these are assertions on RunLoopStats and the trade log rather
// than on anything a caller prints.
//
// No recording is involved. RunLoop takes a vector of prints and an L2Feed, so
// VectorL2Feed lets a case state exactly which frames exist and in what order.

#include "config_parser.hpp"
#include "factory.hpp"
#include "l2_feed.hpp"
#include "papertrader.hpp"
#include "prop_guard.hpp"
#include "run_loop.hpp"
#include "signal_executor.hpp"
#include "trade_event.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace {

// A realistic base. Both loaders reject a timestamp below 1.5e12 and the
// aggregators bucket by wall clock, so an epoch-relative fixture cannot be
// quietly discarded instead of exercised.
constexpr int64_t kBaseMs = 1'700'000'000'000LL;

int64_t At(int64_t offset_ms) { return kBaseMs + offset_ms; }

TradeEvent Print(int64_t offset_ms, double price) {
  TradeEvent t{};
  t.timestamp = At(offset_ms);
  t.price = price;
  t.quantity = 1.0;
  t.is_buyer_maker = false;
  return t;
}

L2Snapshot Frame(int64_t offset_ms, double bid, double ask) {
  L2Snapshot s{};
  s.timestamp = At(offset_ms);
  s.best_bid_price = bid;
  s.best_bid_qty = 1.0;
  s.best_ask_price = ask;
  s.best_ask_qty = 1.0;
  return s;
}

// A backtest-shaped config whose only trigger can never fire. Each case below
// places the position it needs by hand, so a strategy that opened one of its
// own would make the forced-close assertions measure the wrong trade.
AppConfig TestConfig() {
  AppConfig cfg;
  cfg.exchange = "bybit";
  cfg.category = "linear";
  cfg.symbol = "BTCUSDT";
  cfg.sl_pct = 0.002;
  cfg.tp_r = 2.0;
  cfg.tp_pct = cfg.sl_pct * cfg.tp_r;

  ComponentConfig trigger;
  trigger.name = "DeltaAbsorption";
  trigger.params = nlohmann::json{{"min_delta_threshold", 1e18}};
  cfg.triggers.push_back(trigger);
  return cfg;
}

struct Outcome {
  mokuren::RunLoopStats stats;
  std::vector<TradeRecord> trade_log;
};

// One engine, one loop. `before` runs after the engine exists and before the
// loop starts, which is where a case opens a position it wants flattened.
Outcome RunOnce(const std::vector<TradeEvent>& prints,
                const std::vector<L2Snapshot>& book,
                const std::function<void(EngineInstance&)>& before) {
  const AppConfig cfg = TestConfig();
  EngineInstance eng = StrategyFactory::Build(cfg);

  // A default-constructed PropRules is inert, and prop_on=false says so out
  // loud: these cases must not depend on ObserveProp having armed the guard
  // before the first signal.
  const mokuren::PropRules rules{};
  mokuren::PropGuard guard(rules, cfg.initial_balance);

  mokuren::SignalExecutor executor(eng.ptrader.get(), eng.sizer.get(),
                                   eng.risk_manager.get(), cfg.sl_pct,
                                   cfg.tp_pct, "test");

  if (before) before(eng);

  mokuren::VectorL2Feed feed(book);
  Outcome out;
  out.stats = mokuren::RunLoop(eng, executor, guard, /*prop_on=*/false, "test",
                               prints, feed);
  out.trade_log = eng.ptrader->GetTradeHistory();
  return out;
}

// Two-argument form for the cases that need no setup before the loop runs.
// An overload rather than a defaulted `= {}` argument: a brace-initialised
// default on a `const std::function&` parameter is the construct that would
// not parse here, and an empty std::function is false, so the body's
// `if (before)` skips it either way.
Outcome RunOnce(const std::vector<TradeEvent>& prints,
                const std::vector<L2Snapshot>& book) {
  return RunOnce(prints, book, std::function<void(EngineInstance&)>{});
}

// Opens a long that neither the stop nor the target can close during the run,
// so the only thing that can end it is RunLoop's forced close.
void OpenUntouchableLong(EngineInstance& eng, int64_t offset_ms) {
  TradeSignal sig{};
  sig.direction = SignalDirection::BUY;
  sig.entry_price = 100.0;
  sig.stop_loss = 10.0;   // ~90% away: CheckRisk cannot reach it
  sig.take_profit = 1000.0;
  sig.volume = 1.0;
  eng.ptrader->ProcessSignal(sig, 100.0, At(offset_ms));
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. the book tail drain
// ---------------------------------------------------------------------------

TEST_CASE("book frames after the last print are applied, not dropped") {
  // The last print is at 2000; the frame at 2500 exists only in the tail.
  const std::vector<TradeEvent> prints{Print(1000, 100.0), Print(2000, 101.0)};
  const std::vector<L2Snapshot> book{Frame(500, 99.5, 100.5),
                                     Frame(1500, 100.5, 101.5),
                                     Frame(2500, 101.5, 102.5)};

  const Outcome out = RunOnce(prints, book);

  // Without the drain the catch-up stops at the final print, the last applied
  // frame is the 1500 one, and the session reports itself a second short --
  // during which every book-only trigger never fires and the prop guard never
  // sees the final mark.
  CHECK(out.stats.session_end_ts == At(2500));
}

// ---------------------------------------------------------------------------
// 2. session_end_ts drives the forced close
// ---------------------------------------------------------------------------

TEST_CASE("the forced close is timestamped at the last applied book frame") {
  const std::vector<TradeEvent> prints{Print(1000, 100.0), Print(2000, 101.0)};
  const std::vector<L2Snapshot> book{Frame(500, 99.5, 100.5),
                                     Frame(2500, 101.5, 102.5)};

  const Outcome out = RunOnce(prints, book, [](EngineInstance& eng) {
    OpenUntouchableLong(eng, 500);
  });

  REQUIRE(out.trade_log.size() == 1);
  // 2500, the last book frame -- not the final bar's OPEN, which is a whole
  // interval earlier. An early exit_time shifts both the exit marker and the
  // exit bar the inspector draws.
  CHECK(out.trade_log[0].exit_time == At(2500));
  CHECK(out.trade_log[0].direction == SignalDirection::BUY);
  CHECK(out.trade_log[0].exit_price > 0.0);
}

// ---------------------------------------------------------------------------
// 3. the end-price fallback chain
// ---------------------------------------------------------------------------

TEST_CASE("a forced close never prices at zero, even with no book at all") {
  const std::vector<TradeEvent> prints{Print(1000, 100.0), Print(2000, 101.0)};

  const Outcome out = RunOnce(prints, {}, [](EngineInstance& eng) {
    OpenUntouchableLong(eng, 500);
  });

  // No book: session_end_ts stays 0 and the close is timestamped at the bar
  // open. What must not happen is the price -- closing at 0.0 books a phantom
  // total loss, which is one permutation's entire net profit and is invisible
  // in the run's output. Asserted as an invariant rather than against a
  // specific branch: with a nonzero final bar close the first fallback serves
  // it, and the last traded price stands behind that. Either way the number
  // has to be one the run actually saw.
  CHECK(out.stats.session_end_ts == 0);
  REQUIRE(out.trade_log.size() == 1);
  CHECK(out.trade_log[0].exit_price > 99.0);
  CHECK(out.trade_log[0].net_profit > -1.0);
}
