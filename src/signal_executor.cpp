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

// Derives a missing take profit from the stop that will actually be placed.
// This runs after sizing: when the sizer widens the stop, deriving the target
// earlier would leave it anchored to a risk no longer being taken and silently
// change the trade's r-multiple.
bool FinalizeTakeProfit(TradeSignal& sig, double exec_price,
                        double configured_sl_pct, double configured_tp_pct,
                        const char* where) {
  if (sig.take_profit > 0.0) return true;
  if (!(configured_sl_pct > 0.0)) {
    LOG_ERROR("{}: cannot derive take profit from tp_pct={} with sl_pct={}",
              where, configured_tp_pct, configured_sl_pct);
    return false;
  }
  const bool is_long = sig.direction == SignalDirection::BUY;
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

  // 1. stop sanity. also sets entry_price = exec_price.
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

  // 5. take profit anchored to the placed stop.
  if (!FinalizeTakeProfit(final_sig, exec_price, sl_pct_, tp_pct_, where_)) {
    return result;
  }

  if (!broker_) {
    LOG_ERROR("{}: no broker to submit signal", where_);
    return result;
  }

  LOG_TRACE("{}: executing signal dir={} entry={} sl={} tp={} vol={} ts={}",
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
