#pragma once

#include "aggregator.hpp"        // Bar, L2Snapshot, SignalDirection, TradeSignal
#include "ibroker.hpp"           // IBroker, ClosePartial
#include "logger.hpp"            // LOG_WARN
#include "market_context.hpp"    // MarketContext
#include "trade_management.hpp"  // StructuralExitConfig, ExitRuleSpec

#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mokuren {

// What a structural rule decided to do with the open position. Close is the
// only action that changes anything: the guard flattens through
// ClosePartial(1.0), the same path the prop guard already uses. Anything
// finer -- trim half and keep a runner, tighten the stop instead of leaving
// -- needs the position's size and the ladder's state, which are
// PositionManager business, so those actions belong with the v2 rule that
// needs them rather than sitting here as values a v1 build ignores.
enum class ExitAction : uint8_t {
  Hold = 0,
  Close = 1,
};

// A rule's verdict for one evaluation. `reason` is a static string (never a
// temporary) used to build the TradeRecord exit_reason "struct_<rule_name>".
// `arg` is carried for a future action that needs a magnitude; Close ignores
// it.
struct ExitSignal {
  ExitAction   action = ExitAction::Hold;
  double       arg = 0.0;
  const char*  reason = "";
};

// Per-trade state the guard maintains across calls. A rule that needs to
// know the trade's age or its favourable excursion reads this; the guard
// writes it, so a rule cannot persist its own state across an unrelated
// trade. entry_ts_ms < 0 means "the trade clock is not established yet": the
// guard asks IBroker::GetPositionEntryTime() for the real fill time, and
// latches to the first tick it sees only when the broker answers 0 (unknown).
// That fallback is the restart case, and it makes TimeStop / min_holdtime
// measure from process start.
struct ExitState {
  int64_t entry_ts_ms = -1;
  double  mfe = 0.0;   // favourable excursion in PRICE units (positive)
  double  mae = 0.0;   // adverse excursion in PRICE units (positive)
};

// One structural exit rule. Mirrors ITrigger: two hooks so book-only and
// print-only rules live behind the same interface; default bodies return
// Hold so a rule overrides only the hook it cares about. Reset() clears any
// per-trade state and is called by the guard when the position goes flat, so
// a rule with a "seen favourable imbalance yet?" latch starts clean on the
// next trade.
class IExitRule {
public:
  virtual ~IExitRule() = default;
  virtual const char* Name() const = 0;

  virtual ExitSignal OnTrade(const MarketContext& /*ctx*/,
                             const TradeSignal& /*pos*/,
                             double /*price*/, int64_t /*now_ms*/,
                             const ExitState& /*state*/) {
    return {};
  }
  virtual ExitSignal OnBook(const MarketContext& /*ctx*/,
                            const TradeSignal& /*pos*/,
                            int64_t /*now_ms*/,
                            const ExitState& /*state*/) {
    return {};
  }
  virtual void Reset() {}
};

// --- 1R helper ------------------------------------------------------------
//
// Every rule measures against the PLACED stop, matching PositionManager's own
// definition of the risk. Falling back to config.sl_pct would anchor the
// ladder to a number that stops matching the trade the moment a structural
// stop is used (the same failure the BE/trail ladder was moved off).
inline double PlacedRiskOneR(const TradeSignal& pos) {
  return std::abs(pos.entry_price - pos.stop_loss);
}

// --- MfeGiveback ----------------------------------------------------------
//
// Close when the position has given back `giveback_r` of its best favourable
// excursion. Fires only after the trade has actually EARNED giveback_r of
// excursion (mfe_r >= giveback_r), so the rule cannot act as a hidden
// secondary stop loss on a trade that never went anywhere.
class MfeGivebackRule : public IExitRule {
public:
  explicit MfeGivebackRule(double giveback_r) : giveback_r_(giveback_r) {}
  const char* Name() const override { return "MfeGiveback"; }

  ExitSignal OnTrade(const MarketContext&, const TradeSignal& pos,
                     double price, int64_t, const ExitState& state) override {
    const double risk = PlacedRiskOneR(pos);
    if (!(risk > 0.0)) return {};
    const bool is_long = pos.direction == SignalDirection::BUY;
    const double profit = is_long ? price - pos.entry_price
                                  : pos.entry_price - price;
    const double profit_r = profit / risk;
    const double mfe_r    = state.mfe / risk;
    if (mfe_r < giveback_r_) return {};  // nothing to give back yet
    if (profit_r <= mfe_r - giveback_r_) {
      return {ExitAction::Close, 0.0, "MfeGiveback"};
    }
    return {};
  }

private:
  double giveback_r_;
};

// --- TimeStop -------------------------------------------------------------
//
// Close when the trade has lived longer than max_hold_ms. The clock starts at
// the first tick the guard sees with the position open; on a venue restart
// with an existing position, that is the restart time, not the real entry
// time (see ExitState.entry_ts_ms).
class TimeStopRule : public IExitRule {
public:
  explicit TimeStopRule(int64_t max_hold_ms) : max_hold_ms_(max_hold_ms) {}
  const char* Name() const override { return "TimeStop"; }

  ExitSignal OnTrade(const MarketContext&, const TradeSignal&,
                     double, int64_t now_ms, const ExitState& state) override {
    if (state.entry_ts_ms < 0) return {};
    if (now_ms - state.entry_ts_ms >= max_hold_ms_) {
      return {ExitAction::Close, 0.0, "TimeStop"};
    }
    return {};
  }

private:
  int64_t max_hold_ms_;
};

// --- DeltaFlip ------------------------------------------------------------
//
// Close when the bar's cumulative delta has flipped against the position by
// at least min_delta base-asset units. A long is closed by a negative delta
// of -min_delta; a short by a positive delta of +min_delta. Reads live_bar,
// which resets at every candle boundary, so a rule that fires at all fires on
// the strongest reading the current candle offers, not a cumulative one from
// the trade's whole life.
class DeltaFlipRule : public IExitRule {
public:
  explicit DeltaFlipRule(double min_delta) : min_delta_(min_delta) {}
  const char* Name() const override { return "DeltaFlip"; }

  ExitSignal OnTrade(const MarketContext& ctx, const TradeSignal& pos,
                     double, int64_t, const ExitState&) override {
    const double delta = ctx.live_bar.cumulative_delta;
    if (pos.direction == SignalDirection::BUY) {
      if (delta <= -min_delta_) return {ExitAction::Close, 0.0, "DeltaFlip"};
    } else if (pos.direction == SignalDirection::SELL) {
      if (delta >= min_delta_) return {ExitAction::Close, 0.0, "DeltaFlip"};
    }
    return {};
  }

private:
  double min_delta_;
};

// --- ImbalanceBreak -------------------------------------------------------
//
// Close when the top-`min_levels` depth ratio in the position's favour has
// been at or above `ratio` at some point during this trade and has now
// fallen below it. That "at some point" is the whole difference from a
// trigger: entering on imbalance is a filter, exiting on it disappearing is
// a state change, and requires the rule to remember the state it came from.
class ImbalanceBreakRule : public IExitRule {
public:
  ImbalanceBreakRule(double ratio, int min_levels)
      : ratio_(ratio), min_levels_(min_levels) {}
  const char* Name() const override { return "ImbalanceBreak"; }

  void Reset() override { seen_above_ = false; }

  ExitSignal OnBook(const MarketContext& ctx, const TradeSignal& pos,
                    int64_t, const ExitState&) override {
    const double ratio = FavorableImbalance(ctx.latest_l2, pos.direction,
                                            min_levels_);
    if (ratio < 0.0) return {};  // book too shallow to judge
    if (ratio >= ratio_) {
      seen_above_ = true;
      return {};
    }
    if (seen_above_) return {ExitAction::Close, 0.0, "ImbalanceBreak"};
    return {};
  }

private:
  double ratio_;
  int    min_levels_;
  bool   seen_above_ = false;

  // Sum the top min_levels of each side and return the ratio in the
  // position's favour (BUY: bid/ask; SELL: ask/bid). Returns -1.0 when
  // either side has fewer than min_levels real levels, so a shallow book
  // cannot pass.
  static double FavorableImbalance(const L2Snapshot& snap,
                                   SignalDirection dir, int min_levels) {
    double bid_sum = 0.0, ask_sum = 0.0;
    int bid_n = 0, ask_n = 0;
    for (int i = 0; i < MAX_L2_DEPTH && bid_n < min_levels; ++i) {
      if (snap.bids[i].price <= 0.0) break;
      if (snap.bids[i].qty <= 0.0) continue;
      bid_sum += snap.bids[i].qty;
      ++bid_n;
    }
    for (int i = 0; i < MAX_L2_DEPTH && ask_n < min_levels; ++i) {
      if (snap.asks[i].price <= 0.0) break;
      if (snap.asks[i].qty <= 0.0) continue;
      ask_sum += snap.asks[i].qty;
      ++ask_n;
    }
    if (bid_n < min_levels || ask_n < min_levels) return -1.0;
    if (!(bid_sum > 0.0) || !(ask_sum > 0.0)) return -1.0;
    const double bid_ratio = bid_sum / ask_sum;
    return dir == SignalDirection::BUY ? bid_ratio : 1.0 / bid_ratio;
  }
};

// --- SpoofPull ------------------------------------------------------------
//
// Close when a large resting level (>= min_qty) that was present one frame
// ago has lost at least drop_pct of its size, or disappeared entirely. Reads
// l2_history for the previous frame and latest_l2 for the current one. v1
// does not distinguish consumed (trades) from pulled (cancelled) -- it is a
// "the support I was leaning on is gone" heuristic, not a spoof detector --
// and it fires against the side the position leans on (bid for a long, ask
// for a short).
class SpoofPullRule : public IExitRule {
public:
  SpoofPullRule(double drop_pct, double min_qty)
      : drop_pct_(drop_pct), min_qty_(min_qty) {}
  const char* Name() const override { return "SpoofPull"; }

  ExitSignal OnBook(const MarketContext& ctx, const TradeSignal& pos,
                    int64_t, const ExitState&) override {
    if (ctx.l2_history.count < 2) return {};
    const L2Snapshot* prev = ctx.l2_history.get_history(1);
    if (!prev) return {};
    const L2Snapshot& cur = ctx.latest_l2;

    const bool is_long = pos.direction == SignalDirection::BUY;
    const L2Level* prev_side = is_long ? prev->bids : prev->asks;
    const L2Level* cur_side  = is_long ? cur.bids  : cur.asks;

    for (int i = 0; i < MAX_L2_DEPTH; ++i) {
      if (prev_side[i].price <= 0.0) break;
      if (prev_side[i].qty < min_qty_) continue;
      bool found = false;
      for (int j = 0; j < MAX_L2_DEPTH; ++j) {
        if (cur_side[j].price <= 0.0) break;
        if (std::abs(cur_side[j].price - prev_side[i].price) < 1e-12) {
          found = true;
          const double drop = (prev_side[i].qty - cur_side[j].qty) /
                              prev_side[i].qty;
          if (drop >= drop_pct_) return {ExitAction::Close, 0.0, "SpoofPull"};
          break;
        }
      }
      // A level that was >= min_qty and is now gone is a 100% pull.
      if (!found) return {ExitAction::Close, 0.0, "SpoofPull"};
    }
    return {};
  }

private:
  double drop_pct_;
  double min_qty_;
};

// --- DeltaDivergence ------------------------------------------------------
//
// Close when price and cumulative delta have diverged against the position
// over `lookback` bars: for a long, price up but delta down; for a short,
// price down but delta up. A coarse rule (it compares two points, not the
// full path), but the divergence it looks for is exactly the shape of a
// fading move, not a whipsaw.
class DeltaDivergenceRule : public IExitRule {
public:
  explicit DeltaDivergenceRule(int lookback) : lookback_(lookback) {}
  const char* Name() const override { return "DeltaDivergence"; }

  ExitSignal OnTrade(const MarketContext& ctx, const TradeSignal& pos,
                     double, int64_t, const ExitState&) override {
    const auto& hist = ctx.history;
    if (lookback_ <= 0 || static_cast<int>(hist.size()) < lookback_) return {};
    const Bar& older = hist[hist.size() - static_cast<size_t>(lookback_)];
    const Bar& cur = ctx.live_bar;
    if (!(older.close > 0.0) || !(cur.close > 0.0)) return {};

    if (pos.direction == SignalDirection::BUY) {
      const bool price_up   = cur.close > older.close;
      const bool delta_down = cur.cumulative_delta < older.cumulative_delta;
      if (price_up && delta_down) return {ExitAction::Close, 0.0, "DeltaDivergence"};
    } else if (pos.direction == SignalDirection::SELL) {
      const bool price_down = cur.close < older.close;
      const bool delta_up   = cur.cumulative_delta > older.cumulative_delta;
      if (price_down && delta_up) return {ExitAction::Close, 0.0, "DeltaDivergence"};
    }
    return {};
  }

private:
  int lookback_;
};

// --- ExitPlan -------------------------------------------------------------
//
// OR semantics in v1: the first rule to return a non-Hold decision wins.
// A future "mode: all" flag can be added without touching the interface.
class ExitPlan {
public:
  void Add(std::unique_ptr<IExitRule> rule) {
    if (rule) rules_.push_back(std::move(rule));
  }

  bool Empty() const { return rules_.empty(); }

  ExitSignal OnTrade(const MarketContext& ctx, const TradeSignal& pos,
                     double price, int64_t now_ms, const ExitState& state) {
    for (auto& r : rules_) {
      const ExitSignal s = r->OnTrade(ctx, pos, price, now_ms, state);
      if (s.action != ExitAction::Hold) return s;
    }
    return {};
  }

  ExitSignal OnBook(const MarketContext& ctx, const TradeSignal& pos,
                    int64_t now_ms, const ExitState& state) {
    for (auto& r : rules_) {
      const ExitSignal s = r->OnBook(ctx, pos, now_ms, state);
      if (s.action != ExitAction::Hold) return s;
    }
    return {};
  }

  void Reset() {
    for (auto& r : rules_) r->Reset();
  }

private:
  std::vector<std::unique_ptr<IExitRule>> rules_;
};

// --- BuildExitPlan --------------------------------------------------------
//
// Factory side of the load/build split: converts each enabled ExitRuleSpec
// into a concrete IExitRule. Unknown name / missing-or-mistyped param throws
// here, naming the rule and the param. Disabled rules are skipped entirely;
// their params are never read.
namespace detail {

inline double RequireExitNumber(const nlohmann::json& params, const char* key,
                                const char* rule) {
  if (!params.is_object() || !params.contains(key)) {
    throw std::runtime_error(std::string("structural_exit: rule '") + rule +
                             "' requires param '" + key + "'");
  }
  const auto& v = params[key];
  if (v.is_number()) return v.get<double>();
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    size_t used = 0;
    try {
      const double d = std::stod(s, &used);
      if (used == s.size()) return d;
    } catch (...) {}
  }
  throw std::runtime_error(std::string("structural_exit: rule '") + rule +
                           "' param '" + key + "' must be a number (got " +
                           v.dump() + ")");
}

inline int64_t RequireExitInt(const nlohmann::json& params, const char* key,
                              const char* rule) {
  if (!params.is_object() || !params.contains(key)) {
    throw std::runtime_error(std::string("structural_exit: rule '") + rule +
                             "' requires param '" + key + "'");
  }
  const auto& v = params[key];
  if (v.is_number_integer()) return v.get<int64_t>();
  if (v.is_number_float()) {
    const double d = v.get<double>();
    if (std::floor(d) == d) return static_cast<int64_t>(d);
  }
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    size_t used = 0;
    try {
      const int64_t n = std::stoll(s, &used);
      if (used == s.size()) return n;
    } catch (...) {}
  }
  throw std::runtime_error(std::string("structural_exit: rule '") + rule +
                           "' param '" + key + "' must be an integer (got " +
                           v.dump() + ")");
}

} // namespace detail

inline std::unique_ptr<ExitPlan> BuildExitPlan(const StructuralExitConfig& cfg) {
  auto plan = std::make_unique<ExitPlan>();
  if (!cfg.enabled) return plan;

  for (const auto& spec : cfg.rules) {
    if (!spec.enabled) continue;
    if (spec.name.empty()) {
      throw std::runtime_error(
          "structural_exit: a rules[] entry is enabled but has no 'name'");
    }
    if (spec.name == "MfeGiveback") {
      plan->Add(std::make_unique<MfeGivebackRule>(
          detail::RequireExitNumber(spec.params, "giveback_r", "MfeGiveback")));
    } else if (spec.name == "TimeStop") {
      plan->Add(std::make_unique<TimeStopRule>(
          detail::RequireExitInt(spec.params, "max_hold_ms", "TimeStop")));
    } else if (spec.name == "ImbalanceBreak") {
      const double ratio = detail::RequireExitNumber(spec.params, "ratio",
                                                     "ImbalanceBreak");
      const int levels = static_cast<int>(detail::RequireExitInt(
          spec.params, "min_levels", "ImbalanceBreak"));
      plan->Add(std::make_unique<ImbalanceBreakRule>(ratio, levels));
    } else if (spec.name == "DeltaFlip") {
      plan->Add(std::make_unique<DeltaFlipRule>(
          detail::RequireExitNumber(spec.params, "min_delta", "DeltaFlip")));
    } else if (spec.name == "SpoofPull") {
      const double drop = detail::RequireExitNumber(spec.params, "drop_pct",
                                                    "SpoofPull");
      const double min_qty = detail::RequireExitNumber(spec.params, "min_qty",
                                                       "SpoofPull");
      plan->Add(std::make_unique<SpoofPullRule>(drop, min_qty));
    } else if (spec.name == "DeltaDivergence") {
      const int lb = static_cast<int>(detail::RequireExitInt(
          spec.params, "lookback", "DeltaDivergence"));
      plan->Add(std::make_unique<DeltaDivergenceRule>(lb));
    } else {
      throw std::runtime_error(
          "structural_exit: unknown rule '" + spec.name +
          "' (supported: MfeGiveback, TimeStop, ImbalanceBreak, DeltaFlip, "
          "SpoofPull, DeltaDivergence)");
    }
  }
  return plan;
}

// --- StructuralExitGuard --------------------------------------------------
//
// The stateful per-trade object inside PositionManager. Owns the ExitState
// and the ExitPlan; the caller (PositionManager::Update / UpdateOnL2) hands it
// the context, the broker and the clock. A Close verdict is executed here via
// ClosePartial(1.0) -- the same flatten path prop_equity.hpp and the shutdown
// path already use.
class StructuralExitGuard {
public:
  void Configure(const StructuralExitConfig& cfg) {
    spec_ = cfg;
    plan_ = BuildExitPlan(cfg);
    ResetTradeState();
  }

  void ResetTradeState() {
    state_ = ExitState{};
    latched_broker_entry_ = 0;
    if (plan_) plan_->Reset();
  }

  bool Enabled() const { return spec_.enabled && plan_ && !plan_->Empty(); }

  void OnTrade(const MarketContext& ctx, IBroker* broker,
               double price, int64_t now_ms) {
    if (!Enabled() || !broker) return;
    if (!broker->HasOpenPosition()) return;
    const TradeSignal pos = broker->GetCurrentPosition();
    if (pos.direction == SignalDirection::NONE) return;
    if (!(PlacedRiskOneR(pos) > 0.0)) return;

    LatchAndTrack(pos, broker, price, now_ms);

    // min_holdtime_ms is enforced centrally, not per rule: every enabled rule
    // honours it, and there is no secret rule that fires earlier.
    if (now_ms - state_.entry_ts_ms < spec_.min_holdtime_ms) return;

    const ExitSignal sig = plan_->OnTrade(ctx, pos, price, now_ms, state_);
    Execute(sig, broker, price, now_ms);
  }

  void OnBook(const MarketContext& ctx, IBroker* broker, int64_t now_ms) {
    if (!Enabled() || !broker) return;
    if (!broker->HasOpenPosition()) return;
    const TradeSignal pos = broker->GetCurrentPosition();
    if (pos.direction == SignalDirection::NONE) return;

    // A broker that knows the real fill time makes the book path usable before
    // any print has arrived -- the restart case, where the position is already
    // old. Without it the clock latches on a trade tick, as it always did.
    // SyncTradeIdentity may have reset the state just above: a different fill
    // starts over exactly like a new trade, so the clock below is the new
    // trade's, not the one this frame arrived for.
    const int64_t broker_entry = SyncTradeIdentity(broker);
    if (state_.entry_ts_ms < 0) {
      if (broker_entry > 0) state_.entry_ts_ms = broker_entry;
      else return;
    }
    if (now_ms - state_.entry_ts_ms < spec_.min_holdtime_ms) return;

    const ExitSignal sig = plan_->OnBook(ctx, pos, now_ms, state_);

    // A book rule has no print to price the exit from, so it must quote the
    // side a market close would actually cross: a long sells at the bid, a
    // short buys at the ask. The mid is the obvious choice and the wrong one
    // -- it books half a spread better than any fill that exists, and it does
    // so on every book close, so the error accumulates in one direction
    // instead of averaging out.
    //
    // PaperTrader::ClosePartial then applies slippage_pct on top. That is the
    // same treatment a stop-loss gets, and deliberately so: slippage models
    // the gap beyond the price the engine observed, and the price observed
    // here is the touch, not the mid.
    const bool is_long = pos.direction == SignalDirection::BUY;
    double book_price = is_long ? ctx.latest_l2.best_bid_price
                                : ctx.latest_l2.best_ask_price;
    // One-sided or empty book: the touch is 0, and the bar close is the only
    // mark left. Execute refuses a non-positive price either way.
    if (!(book_price > 0.0)) book_price = ctx.live_bar.close;
    Execute(sig, broker, book_price, now_ms);
  }

private:
  StructuralExitConfig      spec_;
  std::unique_ptr<ExitPlan> plan_;
  ExitState                 state_;

  // The fill time of the trade this state belongs to, as the broker reported
  // it; 0 when the broker does not report one. Only a positive value is an
  // identity, which is why 0 is not carried forward as one: it would make the
  // next position's real fill time look like a change when the previous trade
  // was merely unidentifiable.
  int64_t latched_broker_entry_ = 0;

  // Establish, or re-establish, which trade the state describes.
  //
  // A close and a re-open can both land between two Update calls, so
  // PositionManager's flat-tick reset never sees them and the excursion state
  // would be inherited by the next position: an mfe of 2R carried in from the
  // previous trade fires MfeGiveback on the first tick of a trade that never
  // moved -- a hidden stop loss. The fill time is the only trade identity the
  // interface exposes, so a change in it means a different position.
  //
  // A broker that reports 0 cannot be asked; this is a no-op there and the
  // state is reset the old way, by PositionManager observing a flat tick.
  // Returns the broker's answer so the caller can latch the clock without
  // asking twice.
  int64_t SyncTradeIdentity(IBroker* broker) {
    const int64_t broker_entry = broker ? broker->GetPositionEntryTime() : 0;
    if (broker_entry > 0) {
      if (latched_broker_entry_ > 0 && broker_entry != latched_broker_entry_) {
        // A different fill: everything the previous trade taught us is void,
        // including the rule latches and the clock.
        ResetTradeState();
      }
      latched_broker_entry_ = broker_entry;
    }
    return broker_entry;
  }

  void LatchAndTrack(const TradeSignal& pos, IBroker* broker,
                     double price, int64_t now_ms) {
    // SyncTradeIdentity first: it may have reset the state for a trade the
    // guard never saw go flat, and the latch below must then measure THAT
    // trade's age rather than keep the dead one's clock.
    const int64_t broker_entry = SyncTradeIdentity(broker);

    // The trade clock is the broker's answer when it has one: on a restart
    // with a position already open, that is what keeps min_holdtime_ms and
    // TimeStop measuring the trade's real age instead of the process's. A
    // broker that answers 0 (unknown) falls back to the first tick the guard
    // sees, which is the old behaviour.
    if (state_.entry_ts_ms < 0) {
      state_.entry_ts_ms = broker_entry > 0 ? broker_entry : now_ms;
    }
    const bool is_long = pos.direction == SignalDirection::BUY;
    const double favourable = is_long ? (price - pos.entry_price)
                                       : (pos.entry_price - price);
    if (favourable > state_.mfe) state_.mfe = favourable;
    if (-favourable > state_.mae) state_.mae = -favourable;
  }

  void Execute(const ExitSignal& sig, IBroker* broker,
               double price, int64_t now_ms) {
    if (sig.action != ExitAction::Close) return;
    if (!(price > 0.0)) {
      LOG_WARN("StructuralExitGuard: rule '{}' fired but no usable price is "
               "available; not flattening", sig.reason);
      return;
    }
    // The reason carries the rule's own name so TradeRecord.exit_reason
    // reads "struct_MfeGiveback" etc, not "structural" -- the operator can
    // tell which rule fired.
    const std::string reason = std::string("struct_") +
                               (sig.reason ? sig.reason : "unknown");
    broker->ClosePartial(1.0, price, now_ms, reason);
  }
};

} // namespace mokuren
