#pragma once

#include "aggregator.hpp"
#include "ibroker.hpp"

#include <cstdint>

// forward declarations: the funnel only holds pointers, so this header does
// not drag position_sizer.hpp / risk_engine.hpp into every translation unit
// that includes it. both types live in the global namespace.
class PositionSizer;
class RiskManager;

namespace mokuren {

// stop sanity, sets entry_price = exec_price. used before sizing.
bool PrepareSignalRisk(TradeSignal& sig, double exec_price, const char* where);

// anchor the take profit to the stop that will actually be placed. used after
// sizing, so the target follows the placed stop rather than the pre-sizing
// one. when the sizer widened the stop, an existing target is rescaled to keep
// the r-multiple the trigger asked for -- otherwise the trade pays the old,
// much smaller reward for the new, much larger risk.
//
// pre_sizing_risk is |entry - stop| before sizing. 0 means "unknown": a
// missing target is still derived, but an existing one is left untouched.
bool FinalizeTakeProfit(TradeSignal& sig, double exec_price,
                        double configured_sl_pct, double configured_tp_pct,
                        const char* where, double pre_sizing_risk = 0.0);

struct SignalExecutionResult {
  TradeSignal signal{};         // signal actually submitted (NONE if dropped)
  bool submitted = false;       // ProcessSignal was reached
  bool opened_position = false; // flat -> open transition (for TRADE_OPEN)
};

// The execution funnel, in one place. every path that can fire a trade (live
// worker, grid scan, wfa in-sample, wfa out-of-sample, replay) routes through
// this class, so stop validation, sizing, the risk firewall and take-profit
// derivation cannot drift between them.
class SignalExecutor {
public:
  SignalExecutor(IBroker* broker,
                 PositionSizer* sizer,
                 RiskManager* risk_manager,
                 double configured_sl_pct,
                 double configured_tp_pct,
                 const char* where);

  SignalExecutionResult Execute(TradeSignal sig, double exec_price, int64_t ts);

private:
  IBroker* broker_;
  PositionSizer* sizer_;
  RiskManager* risk_manager_;
  double sl_pct_;
  double tp_pct_;
  const char* where_;
};

} // namespace mokuren
