#pragma once
#include "aggregator.hpp"
#include "logger.hpp"
#include "market_context.hpp"
#include <algorithm>
#include <ctime>
#include <vector>
#include <cmath>

// base interface für alle trigger
class ITrigger {
public:
  virtual ~ITrigger() = default;
  virtual TradeSignal EvaluateCandle(const MarketContext &context) = 0;
  virtual TradeSignal EvaluateTick(const MarketContext &context) {
    return TradeSignal{};
  }
};

// erkennt plötzlich gelöschte limit-orders (spoofing) über den l2 ring buffer
class SpoofHunterTrigger : public ITrigger {
private:
  int lookback_ms_;
  double min_wall_qty_;
  double drop_threshold_;
  int depth_levels_ = 5;

public:
  // lookback_ms: wie weit wir zurückschauen (z.b. 500ms)
  // min_wall_qty: mindestgröße der fake-wand (z.b. 30.0 btc)
  // drop_threshold: wie viel der wand verschwinden muss (0.9 = 90%)
  SpoofHunterTrigger(int lookback_ms = 500, double min_wall_qty = 30.0, double drop_threshold = 0.9)
      : lookback_ms_(lookback_ms), min_wall_qty_(min_wall_qty), drop_threshold_(drop_threshold) {}

  TradeSignal EvaluateCandle(const MarketContext &context) override {
    return TradeSignal{}; 
  }

  TradeSignal EvaluateTick(const MarketContext& context) override {
    TradeSignal signal;
    
    // live orderbuch-status abgreifen
    const auto& current_l2 = context.latest_l2;
    if (current_l2.timestamp == 0) return signal;

    // ring buffer nach dem passenden snapshot in der vergangenheit durchsuchen
    const L2Snapshot* past_l2 = nullptr;
    for (size_t i = 0; i < context.l2_history.count; ++i) {
        const auto* snap = context.l2_history.get_history(i);
        
        // stop sobald wir einen snapshot finden, der alt genug ist
        if (snap && (current_l2.timestamp - snap->timestamp) >= lookback_ms_) {
            past_l2 = snap;
            break;
        }
    }

    // noch keine valide historie gefunden (buffer füllt sich noch)
    if (!past_l2) {
        // the ring buffer holds a fixed 1024 frames. a feed that pushes more
        // than 1024 / lookback_ms frames per second never reaches back far
        // enough, and this trigger then stays silent for the whole run -- no
        // signal, no error, indistinguishable from a strategy that found no
        // setups. report the span actually available, the way the other two
        // triggers report theirs.
        const size_t age = context.l2_history.count > 0
                               ? context.l2_history.count - 1
                               : 0;
        const L2Snapshot* oldest = context.l2_history.get_history(age);
        const int64_t span_ms =
            (oldest && current_l2.timestamp > oldest->timestamp)
                ? (current_l2.timestamp - oldest->timestamp)
                : 0;
        LOG_TRACE("spoof_hunter: need {}ms of book history, buffer holds {} "
                  "frames spanning {}ms",
                  lookback_ms_, context.l2_history.count, span_ms);
        return signal;
    }

    // aggregierte orderbuch-tiefe vorbereiten
    double past_bid_qty = 0.0, past_ask_qty = 0.0;
    double current_bid_qty = 0.0, current_ask_qty = 0.0;
    
    // out-of-bounds sicherheitscheck mit der neuen konstanten MAX_L2_DEPTH
    int check_depth = std::min(depth_levels_, MAX_L2_DEPTH);

    // summiere historien-volumen der top-level
    for (int i = 0; i < check_depth; ++i) {
        past_bid_qty += past_l2->bids[i].qty;
        past_ask_qty += past_l2->asks[i].qty;
    }

    // summiere aktuelles volumen der top-level
    for (int i = 0; i < check_depth; ++i) {
        current_bid_qty += current_l2.bids[i].qty;
        current_ask_qty += current_l2.asks[i].qty;
    }

    // check ask spoofing (gestaffelte sell wall gelöscht -> preis schießt hoch -> buy signal)
    if (past_ask_qty >= min_wall_qty_) {
        double drop_limit = past_ask_qty * (1.0 - drop_threshold_);
        if (current_ask_qty <= drop_limit) {
            signal.direction = SignalDirection::BUY;
            return signal;
        }
    }

    // check bid spoofing (gestaffelte buy wall gelöscht -> preis droppt -> sell signal)
    if (past_bid_qty >= min_wall_qty_) {
        double drop_limit = past_bid_qty * (1.0 - drop_threshold_);
        if (current_bid_qty <= drop_limit) {
            signal.direction = SignalDirection::SELL;
            return signal;
        }
    }

    return signal;
  }
};

// Shared stacked-imbalance detection.
//
// Two triggers report this pattern -- one when the candle closes, one while it
// is still building -- and they have to agree on what a stack IS, or the live
// one becomes a different strategy that merely shares a config block. So both
// call the same scan; only the bar they hand it differs.
namespace imbalance {

// One unbroken run of diagonal imbalances, tracked as a price band: the band is
// what a reader can anchor a stop to, and a bare count cannot produce it.
struct StackRun {
  int count = 0;
  double lo = 0.0;
  double hi = 0.0;
  double worst_ratio = 0.0;  // weakest imbalance inside this run
  void Reset() { *this = StackRun{}; }
};

struct Scan {
  StackRun best_buy;
  StackRun best_sell;
  size_t levels = 0;  // so a trace line can say how much was scanned
  double lo = 0.0;
  double hi = 0.0;
  double bid_sum = 0.0;
  double ask_sum = 0.0;
};

// The tick the footprint was built on. Packed levels skip untraded slots, so
// the smallest adjacent delta is a multiple of the real tick and inferring from
// it inflates the gap threshold; the bar carries the real one. The inference is
// only a fallback for callers that build a Bar by hand.
inline double ResolveTick(const Bar &bar) {
  if (bar.tick_size > 0.0) return bar.tick_size;
  double real_tick = 999999.0;
  for (size_t i = 1; i < bar.footprint.size(); i++) {
    const double diff = bar.footprint[i].price - bar.footprint[i - 1].price;
    if (diff > 1e-5 && diff < real_tick) real_tick = diff;
  }
  return real_tick == 999999.0 ? 1.0 : real_tick;
}

// Longest unbroken diagonal imbalance run per side, plus the aggregates the
// trace line needs to be readable.
inline Scan ScanFootprint(const std::vector<PriceLevel> &footprint,
                          double real_tick, double ratio_threshold,
                          double min_level_qty) {
  Scan scan;
  scan.levels = footprint.size();

  StackRun buy_run, sell_run;

  auto commit = [](StackRun &run, StackRun &best) {
    // keep the longest run per side. ties go to the earlier (lower) band,
    // i.e. where the move started rather than where it ended.
    if (run.count > best.count) best = run;
    run.Reset();
  };

  for (size_t i = 1; i < footprint.size(); i++) {
    const auto &upper = footprint[i];
    const auto &lower = footprint[i - 1];

    // a gap or a duplicated level ends the ladder: the diagonal comparison
    // only holds between ADJACENT prices, and PackFootprint drops untraded
    // slots, so a stack must not span one.
    const double diff = upper.price - lower.price;
    if (upper.price == 0.0 || lower.price == 0.0 || diff <= 0.0 ||
        diff > (real_tick * 1.5)) {
      commit(buy_run, scan.best_buy);
      commit(sell_run, scan.best_sell);
      continue;
    }

    // One-sided levels are NOT imbalances. This used to short-circuit
    // ("lower.bid_volume <= 0.0001 || ..."), so a level whose opposite side
    // never traded passed whatever ratio_threshold was: a 3x sweep and a 15x
    // sweep accepted the same ladders, and the parameter could not move the
    // result. A comparison needs two significant volumes.
    const bool upper_bought = upper.ask_volume >= min_level_qty;
    const bool lower_sold = lower.bid_volume >= min_level_qty;
    if (upper_bought && lower_sold &&
        upper.ask_volume >= lower.bid_volume * ratio_threshold) {
      const double ratio = upper.ask_volume / lower.bid_volume;
      if (buy_run.count == 0) {
        buy_run.lo = lower.price;
        buy_run.worst_ratio = ratio;
      } else {
        buy_run.worst_ratio = std::min(buy_run.worst_ratio, ratio);
      }
      buy_run.hi = upper.price;
      buy_run.count++;
    } else {
      commit(buy_run, scan.best_buy);
    }

    const bool upper_sold = upper.bid_volume >= min_level_qty;
    const bool lower_bought = lower.ask_volume >= min_level_qty;
    if (upper_sold && lower_bought &&
        upper.bid_volume >= lower.ask_volume * ratio_threshold) {
      const double ratio = upper.bid_volume / lower.ask_volume;
      if (sell_run.count == 0) {
        sell_run.lo = lower.price;
        sell_run.worst_ratio = ratio;
      } else {
        sell_run.worst_ratio = std::min(sell_run.worst_ratio, ratio);
      }
      sell_run.hi = upper.price;
      sell_run.count++;
    } else {
      commit(sell_run, scan.best_sell);
    }
  }
  // a stack that runs to the last packed level has not been committed yet.
  commit(buy_run, scan.best_buy);
  commit(sell_run, scan.best_sell);

  // the packed levels themselves, not just the stacks they produced. lo/hi
  // separate "one side only" from "levels were merged or split by the tick
  // grid"; bid/ask sums separate an empty tape from a one-sided one.
  for (const auto &lvl : footprint) {
    if (scan.lo == 0.0 || lvl.price < scan.lo) scan.lo = lvl.price;
    if (lvl.price > scan.hi) scan.hi = lvl.price;
    scan.bid_sum += lvl.bid_volume;
    scan.ask_sum += lvl.ask_volume;
  }
  return scan;
}

}  // namespace imbalance

// erkennt gestapelte ungleichgewichte (stacked imbalances) im footprint
class StackedImbalanceTrigger : public ITrigger {
private:
  double ratio_threshold;
  int min_stacked_count;
  // significance floor per footprint level, in base-asset units. 0.0001 was
  // hardcoded, i.e. "not exactly zero", so a single contract against an empty
  // opposite side counted as an infinite imbalance. set this per instrument:
  // ~800 usd of BTC at 80k, a rounding error of SOL.
  double min_level_qty_;
  // the stack has to agree with the candle it closed in. a buy stack on a red
  // candle is buyers being absorbed, i.e. the far side of the move this
  // trigger exists to ride. body direction only: this says nothing about where
  // inside the bar the imbalance sat, and it is deliberately a trigger field
  // rather than a filter -- "the bar agrees with the side that stacked" is a
  // statement about THIS pattern's hypothesis, while a filter states market
  // context that every trigger shares.
  bool require_candle_agreement_;

public:
  // the trailing parameters keep this constructor source-compatible with older
  // call sites while making the noise filters tunable. the defaults are the
  // conservative ones: count only levels that traded on both sides, and do not
  // accept a stack the candle contradicts.
  // require_candle_agreement defaults OFF: whether the candle agrees with the
  // stack is CONTEXT, which is what the filter chain is for (POCTrend,
  // VwapTrend and CVDDivergence already read the same bar). the trigger's job
  // is to report the imbalance and its invalidation, not to decide which
  // market conditions are worth trading. opt in per config when you want it.
  // no cooldown parameter: spacing re-entries is portfolio policy and already
  // lives in the risk modules (AntiRevengeLock). a second copy here made
  // EvaluateCandle stateful, so the same candle could evaluate differently
  // depending on what had fired before it.
  StackedImbalanceTrigger(double ratio, int min_stacked,
                          double min_level_qty = 0.0001,
                          bool require_candle_agreement = false)
      : ratio_threshold(ratio),
        min_stacked_count(min_stacked),
        // a floor of 0 would make every level "significant" and divide by a
        // zero denominator on the next ratio, so clamp instead of trusting it.
        min_level_qty_(min_level_qty > 0.0 ? min_level_qty : 1e-9),
        require_candle_agreement_(require_candle_agreement) {}

  TradeSignal EvaluateCandle(const MarketContext &context) override {
    if (context.history.empty()) return TradeSignal{};
    const Bar &bar = context.history.back();

    if (bar.footprint.size() < 2) return TradeSignal{};

    // the tick and the scan are shared with LiveImbalanceTrigger: it reports
    // the same pattern earlier, and a second copy of this scan would sooner or
    // later disagree with this one about what a stack is.
    const double real_tick = imbalance::ResolveTick(bar);
    const imbalance::Scan scan = imbalance::ScanFootprint(
        bar.footprint, real_tick, ratio_threshold, min_level_qty_);

    const imbalance::StackRun &best_buy = scan.best_buy;
    const imbalance::StackRun &best_sell = scan.best_sell;
    const int max_buy_stacked = best_buy.count;
    const int max_sell_stacked = best_sell.count;

    // one line per evaluated candle: an empty footprint, a run broken by a
    // skipped level and a trigger that was never called all look identical
    // from outside. footprint==0 means this candle had no packed levels at
    // all; max_buy/max_sell are the longest unbroken stacks found. lo/hi and
    // the bid/ask sums come along because without them the counts are
    // unreadable: max_sell=0 on a two-level bid-only footprint is not
    // something the counts alone can explain.
    LOG_TRACE("stacked_imbalance: footprint={} max_buy={} max_sell={} "
              "min_stacked={} ratio={} tick={} lo={} hi={} bid={} ask={} "
              "buy_band=[{},{}] buy_wr={} sell_band=[{},{}] sell_wr={}",
              scan.levels, max_buy_stacked, max_sell_stacked,
              min_stacked_count, ratio_threshold, real_tick, scan.lo, scan.hi,
              scan.bid_sum, scan.ask_sum, best_buy.lo, best_buy.hi,
              best_buy.worst_ratio, best_sell.lo, best_sell.hi,
              best_sell.worst_ratio);

    // both sides stacked means the tape diced back and forth: neither side is
    // the path of least resistance. the old strict ">" tiebreak let this
    // through as silence-by-equality, which is the same outcome for a reason
    // nobody could reconstruct afterwards.
    const bool buy_ok = max_buy_stacked >= min_stacked_count;
    const bool sell_ok = max_sell_stacked >= min_stacked_count;
    if (buy_ok && sell_ok) {
      LOG_TRACE("stacked_imbalance: rejected, both sides stacked (buy={} "
                "sell={})", max_buy_stacked, max_sell_stacked);
      return TradeSignal{};
    }

    const bool want_buy = buy_ok && max_buy_stacked > max_sell_stacked;
    const bool want_sell = sell_ok && max_sell_stacked > max_buy_stacked;

    if (require_candle_agreement_) {
      if (want_buy && !(bar.close > bar.open)) {
        LOG_TRACE("stacked_imbalance: rejected buy stack, candle not up "
                  "(open={} close={})", bar.open, bar.close);
        return TradeSignal{};
      }
      if (want_sell && !(bar.close < bar.open)) {
        LOG_TRACE("stacked_imbalance: rejected sell stack, candle not down "
                  "(open={} close={})", bar.open, bar.close);
        return TradeSignal{};
      }
    }

    const double entry = bar.close;

    // No stop, no target. The trigger reports the PATTERN; the risk geometry
    // is policy and comes from risk.sl_pct / risk.tp_r via SignalExecutor.
    //
    //  - the band is a sub-minute invalidation: ~0.03% from the close on a 1m
    //    bar. At 0.04% taker + 0.02% slippage the round trip costs 3.8R, so a
    //    2R winner nets -1.8R and breakeven winrate exceeds 100%. Nothing said
    //    by the tape survives being paid for at that spread.
    //  - it made risk.sl_pct un-sweepable: sl_pct fed the tp derivation (where
    //    it cancels), the no-stop fallback (which never ran), and
    //    min_distance_dollars (which overrode it). Ten rows, one result.
    //
    // SignalExecutor::Execute fills both levels from the config, and its
    // FinalizeTakeProfit applies tp_r against the stop that actually lands.
    // SpoofHunter has always worked this way.
    auto make_signal = [&](SignalDirection dir, const imbalance::StackRun& run) {
      TradeSignal signal;
      signal.direction = dir;
      signal.entry_price = entry;
      signal.stop_loss = 0.0;    // 0 => funnel applies risk.sl_pct
      signal.take_profit = 0.0;  // 0 => funnel derives tp_r x placed risk
      // the band is still the diagnostic: it is what the trigger DETECTED on,
      // so a reader can see how far the policy stop sits from the level that
      // fired the signal.
      LOG_TRACE("stacked_imbalance: {} levels={} band=[{},{}] wr={} entry={} "
                "(stop and target deferred to risk config)",
                dir == SignalDirection::BUY ? "buy" : "sell", run.count, run.lo,
                run.hi, run.worst_ratio, entry);
      return signal;
    };

    if (want_buy) return make_signal(SignalDirection::BUY, best_buy);
    if (want_sell) return make_signal(SignalDirection::SELL, best_sell);

    return TradeSignal{};
  }
};

// Reports the stacked-imbalance pattern while the candle is still building.
//
// StackedImbalanceTrigger evaluates history.back(), i.e. a bar that only exists
// after the candle has closed, so the setup -- aggressive flow stacking on the
// footprint diagonal -- is already half a move old when it is reported. The
// pattern itself is visible much earlier: it exists as soon as a few adjacent
// levels have printed with the required ratio between them.
//
// This works only because the aggregator now mirrors the grid into the live bar
// (Bar::UpsertFootprintLevel), and it reports through the same
// imbalance::ScanFootprint the close-time trigger uses, so the two cannot drift
// into disagreeing about what a stack is.
//
// Edge-triggered, not level-triggered: EvaluateTick runs on every print and a
// stack does not un-form between prints, so reading it level-triggered would
// emit an identical ticket for the rest of the candle. A side reports once when
// its run first qualifies, and re-arms only when that run breaks -- which keeps
// a break-and-reform as the second setup it is.
//
// No candle-agreement option: the body it would check is unfinished, so
// "close > open" mid-candle says nothing about where the imbalance sat.
class LiveImbalanceTrigger : public ITrigger {
private:
  double ratio_threshold_;
  int min_stacked_count_;
  // significance floor per footprint level, in base-asset units. Set this per
  // instrument: the first prints of a candle build a thin footprint, and a
  // floor that is effectively zero makes three rounding errors into a stack.
  double min_level_qty_;

  // one formation is one signal; cleared when the run breaks.
  bool buy_latched_ = false;
  bool sell_latched_ = false;
  int64_t latched_bar_ts_ = 0;
  // the both-sided rejection repeats on every print until one side dissolves;
  // report the first one per candle and stay quiet afterwards.
  bool both_warned_ = false;

public:
  LiveImbalanceTrigger(double ratio, int min_stacked,
                       double min_level_qty = 0.0001)
      : ratio_threshold_(ratio),
        min_stacked_count_(min_stacked),
        // a floor of 0 would make every level "significant" and divide by a
        // zero denominator on the next ratio, so clamp instead of trusting it.
        min_level_qty_(min_level_qty > 0.0 ? min_level_qty : 1e-9) {}

  TradeSignal EvaluateCandle(const MarketContext &context) override {
    // close-time reporting is StackedImbalanceTrigger's job. reporting the same
    // stack here as well would put two tickets on one setup.
    return TradeSignal{};
  }

  TradeSignal EvaluateTick(const MarketContext &context) override {
    const Bar &bar = context.live_bar;

    // a rolled candle starts from an empty footprint, but its first prints may
    // already qualify; the latches must not outlive their candle.
    if (bar.timestamp_start != latched_bar_ts_) {
      latched_bar_ts_ = bar.timestamp_start;
      buy_latched_ = false;
      sell_latched_ = false;
      both_warned_ = false;
    }

    // close==0 until UpdateBarData has seen a print for this candle, and a
    // signal carrying entry 0 is a zero-price ticket, not a setup.
    if (bar.close <= 0.0 || bar.footprint.size() < 2) return TradeSignal{};

    const double real_tick = imbalance::ResolveTick(bar);
    const imbalance::Scan scan = imbalance::ScanFootprint(
        bar.footprint, real_tick, ratio_threshold_, min_level_qty_);

    const bool buy_ok = scan.best_buy.count >= min_stacked_count_;
    const bool sell_ok = scan.best_sell.count >= min_stacked_count_;

    // re-arm a side whose run no longer qualifies.
    if (!buy_ok) buy_latched_ = false;
    if (!sell_ok) sell_latched_ = false;

    // both sides stacked: the tape diced back and forth, neither side is the
    // path of least resistance. same rule as the close-time trigger, and
    // deliberately not latched -- whichever side dissolves first is a setup.
    if (buy_ok && sell_ok) {
      if (!both_warned_) {
        LOG_TRACE("live_imbalance: rejected, both sides stacked (buy={} "
                  "sell={}) ts={}", scan.best_buy.count, scan.best_sell.count,
                  bar.timestamp_start);
        both_warned_ = true;
      }
      return TradeSignal{};
    }

    const bool want_buy = buy_ok && !buy_latched_;
    const bool want_sell = sell_ok && !sell_latched_;
    if (!want_buy && !want_sell) return TradeSignal{};

    if (want_buy) {
      buy_latched_ = true;
    } else {
      sell_latched_ = true;
    }

    const imbalance::StackRun &run = want_buy ? scan.best_buy : scan.best_sell;

    TradeSignal signal;
    signal.direction = want_buy ? SignalDirection::BUY : SignalDirection::SELL;
    signal.entry_price = bar.close;
    // same contract as the close-time trigger and SpoofHunter: the trigger
    // reports the pattern, the funnel applies risk.sl_pct and risk.tp_r.
    signal.stop_loss = 0.0;    // 0 => funnel applies risk.sl_pct
    signal.take_profit = 0.0;  // 0 => funnel derives tp_r x placed risk

    LOG_TRACE("live_imbalance: {} fired levels={} band=[{},{}] wr={} entry={} "
              "vol={} footprint={} ts={} (stop and target deferred to risk "
              "config)",
              want_buy ? "buy" : "sell", run.count, run.lo, run.hi,
              run.worst_ratio, bar.close, bar.total_volume, scan.levels,
              bar.timestamp_start);
    return signal;
  }
};

// erkennt absorption (preis bewegt sich gegen massiven aggressiven orderflow)
class DeltaAbsorptionTrigger : public ITrigger {
private:
  double min_delta_threshold_;

public:
  // a threshold of 0 (or below) reads as "fire on any net delta", which is
  // every candle whose close agrees with the delta sign. the configs carried
  // a min_delta_threshold grid whose first step was 0, and a zero there is
  // indistinguishable from "no filter at all" in the trade count. the factory
  // validates config values and rejects <= 0; the floor here only protects a
  // direct caller (tests, tooling) that skips the factory.
  DeltaAbsorptionTrigger(double min_delta)
      : min_delta_threshold_(min_delta > 0.0 ? min_delta : 1e-9) {}

  TradeSignal EvaluateCandle(const MarketContext &context) override {
    if (context.history.empty()) return TradeSignal{};
    const Bar &bar = context.history.back();

    double total_delta = 0.0;
    
    // netto-delta aus dem footprint berechnen
    for (const auto &level : bar.footprint) {
      if (level.price > 0.0) {
        total_delta += (level.ask_volume - level.bid_volume);
      }
    }

    // a candle close is the only place this trigger can ever speak, so one
    // line here is the whole diagnostic: a threshold no tape can reach looks
    // identical to a strategy that never armed.
    LOG_TRACE("delta_absorption: delta={} threshold={} open={} close={} vol={}",
              total_delta, min_delta_threshold_, bar.open, bar.close,
              bar.total_volume);

    // buy signal: massiver verkaufsdruck aber kerze schließt grün
    if (total_delta <= -min_delta_threshold_ && bar.close > bar.open) {
      TradeSignal ticket;
      ticket.direction = SignalDirection::BUY;
      ticket.entry_price = bar.close; 
      ticket.stop_loss = bar.low - (bar.tick_size * 2);

      // the target is deliberately left at 0: SignalExecutor::FinalizeTakeProfit
      // derives it as risk.tp_r x the placed risk, so tp_r is actually
      // configurable. the old hardcoded 2R was preserved by the funnel's
      // rescale, so a config that said tp_r: 3 still traded 2R on this trigger
      // -- and nothing in the log said so. the stop stays structural
      // (bar.low - 2 ticks): that is the invalidation this setup has evidence
      // for.
      ticket.take_profit = 0.0;  // 0 => funnel derives tp_r x placed risk

      return ticket;
    }

    // sell signal: massiver kaufdruck aber kerze schließt rot
    // mirrored from the long branch: a direction-only signal carries no stop,
    // and the engine's fail-closed funnel drops it before it can trade.
    if (total_delta >= min_delta_threshold_ && bar.close < bar.open) {
      TradeSignal ticket;
      ticket.direction = SignalDirection::SELL;
      ticket.entry_price = bar.close;
      ticket.stop_loss = bar.high + (bar.tick_size * 2);

      // see the long branch: tp_r comes from config via the funnel, not from a
      // hardcoded multiple here.
      ticket.take_profit = 0.0;  // 0 => funnel derives tp_r x placed risk

      return ticket;
    }

    return TradeSignal{};
  }
};

// logischer or-block (erstes valides signal gewinnt)
class OR_Trigger : public ITrigger {
private:
  std::vector<ITrigger *> triggers_;

public:
  void AddTrigger(ITrigger *trigger) { triggers_.push_back(trigger); }

  TradeSignal EvaluateCandle(const MarketContext &context) override {
    for (auto *t : triggers_) {
      TradeSignal signal = t->EvaluateCandle(context);
      if (signal.direction != SignalDirection::NONE) {
        return signal;
      }
    }
    return TradeSignal{};
  }

  TradeSignal EvaluateTick(const MarketContext &context) override {
    for (auto *t : triggers_) {
      TradeSignal signal = t->EvaluateTick(context);
      if (signal.direction != SignalDirection::NONE)
        return signal;
    }
    return TradeSignal{};
  }
};

// logischer and-block (erfordert einstimmigen konsens aller sub-trigger)
class AND_Trigger : public ITrigger {
private:
  std::vector<ITrigger *> triggers_;

public:
  void AddTrigger(ITrigger *trigger) { triggers_.push_back(trigger); }

  TradeSignal EvaluateCandle(const MarketContext &context) override {
    if (triggers_.empty()) return TradeSignal{};

    // basis-konsens vom ersten trigger einholen
    TradeSignal consensus = triggers_[0]->EvaluateCandle(context);
    if (consensus.direction == SignalDirection::NONE) return TradeSignal{};

    // konsens über restliche trigger verifizieren
    for (size_t i = 1; i < triggers_.size(); ++i) {
      if (triggers_[i]->EvaluateCandle(context).direction != consensus.direction) {
        return TradeSignal{}; 
      }
    }

    return consensus; 
  }
};
