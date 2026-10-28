#include "signal_executor.hpp"

#include "logger.hpp"
#include "position_sizer.hpp"
#include "risk_engine.hpp"

#include <cmath>

namespace mokuren {

// A signal's own stop is the risk actually taken, so it defines 1R. The engine
// never overwrites it with risk.sl_pct: doing so would silently redefine every
// r-multiple threshold for a strategy that chose a structural stop, and make
// the backtest disagree with the placed-stop ladder used live. A missing or
// inverted stop drops the signal instead of inventing a price.
bool PrepareSignalRisk(TradeSignal& sig, double exec_price, const char* where) {
  const bool is_long = sig.direction == SignalDirection::BUY;
  if (!(sig.stop_loss > 0.0) ||
      (is_long && sig.stop_loss >= exec_price) ||
      (!is_long && sig.stop_loss <= exec_price)) {
    LOG_ERROR("{}: dropping {} signal at {}: stop_loss={} is missing or on "
              "the wrong side of entry",
              where, is_long ? "long" : "short", exec_price, sig.stop_loss);
    return false;
  }
  sig.entry_price = exec_price;
  return true;
}

// Anchors the take profit to the stop that will actually be placed. Runs after
// sizing: when the sizer widens the stop, a target derived or left behind
// earlier is anchored to a risk no longer being taken.
bool FinalizeTakeProfit(TradeSignal& sig, double exec_price,
                        double configured_sl_pct, double configured_tp_pct,
                        const char* where, double pre_sizing_risk) {
  const bool is_long = sig.direction == SignalDirection::BUY;
  const double placed_risk = std::abs(exec_price - sig.stop_loss);

  if (sig.take_profit > 0.0) {
    // a target on the wrong side of entry cannot be repaired by scaling it.
    // drop it here with a named reason instead of handing the broker a trade
    // that is certain to be rejected.
    if ((is_long && sig.take_profit <= exec_price) ||
        (!is_long && sig.take_profit >= exec_price)) {
      LOG_ERROR("{}: dropping {} signal at {}: take_profit={} is on the wrong "
                "side of entry",
                where, is_long ? "long" : "short", exec_price, sig.take_profit);
      return false;
    }

    // keep the trigger's reward:risk. the sizer may have widened the stop to
    // honour risk.min_distance_dollars; leaving the target where the trigger
    // put it would pay the old, much smaller reward for the new, much larger
    // risk (a 50 usd stop against a 3 usd target is a 0.07:1 trade). scaling
    // by the ratio of placed to original risk is a no-op when the stop was
    // not moved, so this does not need to know whether it was.
    if (pre_sizing_risk > 0.0 && placed_risk > 0.0) {
      const double reward = std::abs(sig.take_profit - exec_price);
      const double r_multiple = reward / pre_sizing_risk;
      const double new_tp = is_long
                                ? exec_price + r_multiple * placed_risk
                                : exec_price - r_multiple * placed_risk;
      if (std::abs(new_tp - sig.take_profit) > std::abs(exec_price) * 1e-9) {
        LOG_DEBUG("{}: rescaling {} target {} -> {} to keep {:.2f}R (risk {} "
                  "-> {})",
                  where, is_long ? "long" : "short", sig.take_profit, new_tp,
                  r_multiple, pre_sizing_risk, placed_risk);
        sig.take_profit = new_tp;
      }
    }
    return true;
  }

  if (!(configured_sl_pct > 0.0)) {
    LOG_ERROR("{}: cannot derive take profit from tp_pct={} with sl_pct={}",
              where, configured_tp_pct, configured_sl_pct);
    return false;
  }
  // tp_pct was resolved as a pct of entry against risk.sl_pct, so dividing
  // recovers the r; multiplying by the placed risk keeps the target at the
  // same r-multiple of the stop that actually gets placed.
  const double risk_pct = std::abs(exec_price - sig.stop_loss) / exec_price;
  const double tp_pct = (configured_tp_pct / configured_sl_pct) * risk_pct;
  sig.take_profit =
      is_long ? exec_price * (1.0 + tp_pct) : exec_price * (1.0 - tp_pct);
  return true;
}

SignalExecutor::SignalExecutor(IBroker* broker,
                               PositionSizer* sizer,
                               RiskManager* risk_manager,
                               double configured_sl_pct,
                               double configured_tp_pct,
                               const char* where)
    : broker_(broker),
      sizer_(sizer),
      risk_manager_(risk_manager),
      sl_pct_(configured_sl_pct),
      tp_pct_(configured_tp_pct),
      where_(where ? where : "signal_executor") {}

SignalExecutionResult SignalExecutor::Execute(TradeSignal sig, double exec_price,
                                              int64_t ts) {
  SignalExecutionResult result{};

  if (sig.direction == SignalDirection::NONE) return result;

  // 1. stop: a direction-only ticket (SpoofHunter, StackedImbalance) carries no
  //    levels, so fall back to risk.sl_pct -- that is exactly 1R, and the
  //    target is derived from it by tp_r downstream. an explicit stop chosen
  //    by the trigger (DeltaAbsorption) is never overwritten: the level it
  //    measured is the risk actually being taken. then the sanity check, which
  //    also sets entry_price = exec_price.
  if (!(sig.stop_loss > 0.0)) {
    const bool long_side = sig.direction == SignalDirection::BUY;
    if (!(sl_pct_ > 0.0)) {
      LOG_ERROR("{}: dropping {} signal: ticket has no stop and sl_pct={}",
                where_, long_side ? "long" : "short", sl_pct_);
      return result;
    }
    sig.stop_loss = long_side ? exec_price * (1.0 - sl_pct_)
                              : exec_price * (1.0 + sl_pct_);
    LOG_DEBUG("{}: {} ticket has no stop, defaulting to sl_pct={} -> {}",
              where_, long_side ? "long" : "short", sl_pct_, sig.stop_loss);
  }
  if (!PrepareSignalRisk(sig, exec_price, where_)) return result;

  // 2. sizing. may widen the stop to honour the configured minimum distance.
  TradeSignal sized = sig;
  if (sizer_) sized = sizer_->CalculateSize(sig);

  // 3. risk firewall. returns NONE on any module veto.
  TradeSignal final_sig = sized;
  if (risk_manager_) final_sig = risk_manager_->Evaluate(sized, ts);
  if (final_sig.direction == SignalDirection::NONE) return result;

  // 4. re-validate the stop AFTER sizing and the firewall. this used to be
  //    live-only, which meant a risk module that moved a stop to the wrong
  //    side was caught live and silently accepted in the scan/wfa.
  const bool final_long = final_sig.direction == SignalDirection::BUY;
  if (!(final_sig.stop_loss > 0.0) ||
      (final_long && final_sig.stop_loss >= exec_price) ||
      (!final_long && final_sig.stop_loss <= exec_price)) {
    LOG_ERROR("{}: dropping {} signal at {}: final stop_loss={} is missing "
              "or on the wrong side of entry",
              where_, final_long ? "long" : "short", exec_price,
              final_sig.stop_loss);
    return result;
  }

  // 5. take profit anchored to the placed stop. the pre-sizing risk lets an
  //    existing target keep its r-multiple when the stop was widened to honour
  //    risk.min_distance_dollars.
  const double pre_sizing_risk = std::abs(sig.entry_price - sig.stop_loss);
  if (!FinalizeTakeProfit(final_sig, exec_price, sl_pct_, tp_pct_, where_,
                          pre_sizing_risk)) {
    return result;
  }

  if (!broker_) {
    LOG_ERROR("{}: no broker to submit signal", where_);
    return result;
  }

  LOG_DEBUG("{}: executing signal dir={} entry={} sl={} tp={} vol={} ts={}",
            where_, static_cast<int>(final_sig.direction),
            final_sig.entry_price, final_sig.stop_loss, final_sig.take_profit,
            final_sig.volume, ts);

  // 6. submit. capture the flat -> open transition so callers that need it
  //    (the live orchestrator, for TRADE_OPEN) do not re-query the broker.
  const bool was_open = broker_->HasOpenPosition();
  broker_->ProcessSignal(final_sig, exec_price, ts);
  const bool is_open = broker_->HasOpenPosition();

  result.signal = final_sig;
  result.submitted = true;
  result.opened_position = (!was_open && is_open);
  return result;
}

} // namespace mokuren
