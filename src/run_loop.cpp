#include "run_loop.hpp"

#include "aggregator.hpp"
#include "factory.hpp"           // EngineInstance
#include "ibroker.hpp"
#include "market_context.hpp"
#include "prop_equity.hpp"
#include "prop_guard.hpp"
#include "signal_executor.hpp"

namespace mokuren {

RunLoopStats RunLoop(EngineInstance& eng, SignalExecutor& executor,
                     PropGuard& prop_guard, bool prop_on, const char* where,
                     const std::vector<TradeEvent>& trades, L2Feed& l2) {
  RunLoopStats stats;
  Bar live_bar;
  Bar htf_bar;
  // Last traded price, used as the execution fallback for a book frame whose
  // side is empty and for the forced close when the final bar has no close.
  double last_price = 0.0;

  // One book frame, applied in the order the live path applies it: both
  // aggregators, then the guard observer, then the tick-time evaluation.
  // Split out because it runs from two places (the per-trade catch-up and the
  // tail drain) and they must not drift.
  auto apply_book = [&](const L2Snapshot& snap, double fallback_price) {
    eng.aggregator->ProcessL2(snap);
    eng.htf_aggregator->ProcessL2(snap);

    if (snap.timestamp > stats.session_end_ts) {
      stats.session_end_ts = snap.timestamp;
    }

    // Observe the guard on book frames as well as prints: a floating loss can
    // cross a limit on a quiet book with no prints, and the live path observes
    // it there too.
    const PropDecision pd =
        ObservePropBook(prop_guard, eng.ptrader.get(), snap.best_bid_price,
                        snap.best_ask_price, fallback_price, snap.timestamp,
                        where);
    if (pd.state == PropState::DailyHalted ||
        pd.state == PropState::TotalHalted) {
      stats.prop_breached = true;
    }

    MarketContext tick_ctx{eng.aggregator->GetHistory(), live_bar, htf_bar,
                           eng.aggregator->GetSessionMetrics(),
                           eng.aggregator->GetLatestL2(),
                           eng.aggregator->GetL2History()};

    const TradeSignal tick_sig = eng.alpha->Evaluate(tick_ctx, false);
    if (tick_sig.direction == SignalDirection::NONE) return;

    ++stats.signals_seen;
    if (!prop_on || prop_guard.allow_new_entry()) {
      // Cross the spread: a BUY lifts the ask, a SELL hits the bid. Fall back
      // to the last candle close, then to the price of the print this frame is
      // being caught up for -- an empty side must not hand the sizer a 0.0
      // entry price.
      double exec_price = (tick_sig.direction == SignalDirection::BUY)
                              ? snap.best_ask_price
                              : snap.best_bid_price;
      if (!(exec_price > 0.0)) {
        exec_price = live_bar.close > 0.0 ? live_bar.close : fallback_price;
      }
      const SignalExecutionResult r =
          executor.Execute(tick_sig, exec_price, snap.timestamp);
      if (r.signal.direction != SignalDirection::NONE) {
        ++stats.signals_executed;
      }
      if (r.opened_position) ++stats.signals_opened;
    } else {
      ++stats.signals_vetoed;
    }
  };

  for (const auto& trade : trades) {
    // Catch the book up -- strict `<`, matching the live streamer, which emits
    // the print before the book frame at identical timestamps. `<=` would hand
    // the strategy a book the live run had not seen yet.
    while (l2.HasNext() && l2.Front().timestamp < trade.timestamp) {
      apply_book(l2.Front(), trade.price);
      l2.Advance();
    }

    last_price = trade.price;

    // Observe the guard before anything can act on this print: a breach must
    // refuse the trade, not race it.
    const PropDecision pd = ObserveProp(prop_guard, eng.ptrader.get(),
                                        trade.price, trade.timestamp, where);
    if (pd.state == PropState::DailyHalted ||
        pd.state == PropState::TotalHalted) {
      stats.prop_breached = true;
    }

    eng.pos_manager->Update(trade.price, trade.timestamp);
    eng.ptrader->CheckRisk(trade.price, trade.timestamp);

    const bool candle_finished = eng.aggregator->ProcessTrade(live_bar, trade);
    eng.htf_aggregator->ProcessTrade(htf_bar, trade);

    MarketContext context{eng.aggregator->GetHistory(), live_bar, htf_bar,
                          eng.aggregator->GetSessionMetrics(),
                          eng.aggregator->GetLatestL2(),
                          eng.aggregator->GetL2History()};

    // Execute() is only reached for a real signal. One caller used to pass a
    // NONE signal straight through; PrepareSignalRisk rejects it on the missing
    // stop, so this is the same outcome without the wasted call.
    const TradeSignal candle_sig =
        eng.alpha->Evaluate(context, candle_finished);
    if (candle_sig.direction == SignalDirection::NONE) continue;

    ++stats.signals_seen;
    if (!prop_on || prop_guard.allow_new_entry()) {
      const SignalExecutionResult r =
          executor.Execute(candle_sig, trade.price, trade.timestamp);
      if (r.signal.direction != SignalDirection::NONE) {
        ++stats.signals_executed;
      }
      if (r.opened_position) ++stats.signals_opened;
    } else {
      ++stats.signals_vetoed;
    }
  }

  // The tail. Book frames after the final print used to be dropped, because the
  // catch-up only ran while a trade existed to catch up for -- so a recording
  // whose book outlives its last trade truncated at that trade. There is no
  // current print to fall back to here, so the last one stands in.
  while (l2.HasNext()) {
    apply_book(l2.Front(), last_price);
    l2.Advance();
  }

  eng.aggregator->FlushLastCandle(live_bar);

  double end_price = live_bar.close;
  if (!(end_price > 0.0)) end_price = last_price;
  if (!(end_price > 0.0) && !eng.aggregator->GetHistory().empty()) {
    end_price = eng.aggregator->GetHistory().back().close;
  }
  // `session_end_ts` is the last book frame actually applied. When the run saw
  // no book at all, the bar open is the only timestamp available.
  const int64_t end_ts = stats.session_end_ts > 0 ? stats.session_end_ts
                                                  : live_bar.timestamp_start;
  eng.ptrader->CloseOpenPositionAtEnd(end_price, end_ts);

  return stats;
}

}  // namespace mokuren
