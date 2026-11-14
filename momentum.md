# Momentum Exit Gameplan — Structural Exits v1

## Status
Plan finalized. Ready to code. All questions resolved.

## Goal
Add configurable "structural exit" rules that watch an open position and
close it when the market structure that justified the entry disappears.
Entries are untouched. The fixed SL remains a hard floor. Everything else
(TP, BE, trailing, scale-out, prop guard) continues to work exactly as it
does today.

## Non-goals
- No change to entry triggers, filters, sizing, or risk modules.
- No change to `TradeSignal`, `ITrigger`, `SignalExecutor`, `PropGuard`.
- No change to `IBroker` — closes go through `ClosePartial(1.0, ...)`,
  which is the established flatten path used by `prop_equity.hpp` and
  `LiveOrchestrator::CheckPropGuardEquity`.
- No change to `MarketContext`.
- No in-place editing of existing config keys.

## Architecture overview

Structural exits are a **third, parallel close path** on an open position:

| Close path             | Trigger                              | Owner today        |
|------------------------|--------------------------------------|--------------------|
| SL / TP                | price reaches a level                | broker `CheckRisk` |
| BE / trail / scale-out | profit reaches an R multiple         | `PositionManager`  |
| Prop guard             | account drawdown breach              | `PropGuard`        |
| **Structural**         | market structure / trade age / MFE   | `StructuralExitGuard` |

Whoever closes first wins. Each consumer checks `!HasOpenPosition()` before
acting. No consumer overrides another's configuration; they share the
position, not the decision logic.

### Interfaces

- `IExitRule` — virtual, mirrors `ITrigger`. Two hooks (`OnTrade`, `OnBook`)
  so book-only and print-only rules live in the same subsystem. Default
  bodies return `{}` so a rule only overrides the hook it cares about.
- `ExitPlan` — holds `vector<unique_ptr<IExitRule>>`, iterates, first
  non-`Hold` wins. V1 is OR semantics. A future `mode: "all"` flag can be
  added without interface change.
- `StructuralExitGuard` — the stateful per-trade object inside
  `PositionManager`. Owns `ExitState` (`entry_ts_ms`, `mfe`, `mae`) and the
  `ExitPlan`. `TradeSignal` has no `entry_time` field, so the guard must
  own it.
- `StructuralExitConfig` — plain data struct, filled by the resolver.
- `ExitSignal` — `{ExitAction, arg, reason}`. Only `Hold` and `Close` are
  wired in v1; `Reduce` and `TightenStop` exist in the enum so v2 can add
  rules without touching callers, and log a `LOG_WARN` if ever returned.

### Why the close verb is `ClosePartial(1.0)`
`prop_equity.hpp:52-63, 80-82` and `live_engine.cpp:705` already use
`ClosePartial(1.0, price, ts, "prop_guard")` as the canonical flatten.
PaperTrader (`papertrader.cpp:318-321`) explicitly designs `ClosePartial(1.0)`
to leave the same state as a full close. All three venue adapters implement
it. Adding a new `IBroker::ClosePosition` would only be worth it if we also
migrated those paths — zero behavioural gain for scope creep.

## Config schema

Optional block, sibling of `break_even` / `trailing` / `scale_out`:

```json
"trade_management": {
  "break_even": { "trigger_r": 1.0, "target_r": 0.2 },
  "trailing":   { "enabled": true, "trigger_r": 1.5, "distance_r": 0.5 },
  "scale_out":  { "enabled": false, "trigger_r": 1.2, "fraction": 0.5 },
  "structural_exit": {
    "enabled": true,
    "min_holdtime_ms": 5000,
    "rules": [
      { "name": "MfeGiveback",     "enabled": true,  "giveback_r": 0.5 },
      { "name": "ImbalanceBreak",  "enabled": true,  "ratio": 2.5, "min_levels": 3 },
      { "name": "DeltaFlip",       "enabled": false, "min_delta": 50.0 },
      { "name": "TimeStop",        "enabled": true,  "max_hold_ms": 900000 },
      { "name": "SpoofPull",       "enabled": false },
      { "name": "DeltaDivergence", "enabled": false }
    ]
  }
}
```

Rules:
- **`structural_exit.enabled`** — master switch (default `false`). Absent
  block or `enabled=false` → feature off, existing configs unchanged.
- **`min_holdtime_ms`** — **required** (must be `> 0`) when the master
  switch is on. Reject at load. No silent default — as the author put it:
  without it this is a suicide mission.
- **`rules[i].enabled`** — per-rule switch. Absent → defaults to `true`.
  Disabled rules are skipped at build time and their params are not
  validated, matching `IsDisabled` / `MakeTrigger` in `src/factory.cpp`.
  Accepted spellings: `enabled` / `active` / `isActive`, each as bool
  `false`, `"false"` / `"0"` string, or integer `0` — same as triggers,
  filters, risk modules.
- **`rules[]`** — same `{name, params}` shape as `triggers`. Unknown
  `name` throws at load. Missing required param on an **enabled** rule
  throws at load; a disabled rule's params are never read.
- Any enabled rule returning `Close` closes the position; OR semantics
  in v1.
- A rule that needs no parameters may be listed as `{"name": "..."}`.
- `enabled=true` with **all rules disabled** (or an empty `rules[]`)
  throws at load, mirroring `AppConfig::Validate`'s "every trigger is
  disabled" error at `config_parser.cpp:381-395`.

## Rule catalogue (v1)

| Rule              | Hook    | Params                | Reads                                             |
|-------------------|---------|-----------------------|---------------------------------------------------|
| `MfeGiveback`     | OnTrade | `giveback_r`          | trade price, `ExitState.mfe`, `pos.stop_loss` (1R) |
| `TimeStop`        | OnTrade | `max_hold_ms`         | `ExitState.entry_ts_ms`, clock                    |
| `ImbalanceBreak`  | OnBook  | `ratio`, `min_levels` | `ctx.l2_history`, `ctx.live_bar`                  |
| `DeltaFlip`       | OnTrade | `min_delta`           | `ctx.live_bar` (delta field)                      |
| `SpoofPull`       | OnBook  | `drop_pct`, `min_qty` | `ctx.l2_history`, `ctx.latest_l2`                 |
| `DeltaDivergence` | OnTrade | `lookback`            | `ctx.history`, `ctx.live_bar`                     |

1R for `MfeGiveback` is `|pos.entry_price - pos.stop_loss|`, matching
`position_manager.hpp:100-104`'s own definition of the placed risk.
`min_holdtime_ms` is enforced centrally in `StructuralExitGuard`, not per
rule — every enabled rule must honour it.

## Validation split — load time vs build time

Mirrors the trigger pipeline exactly:

- **Load time** (`ResolveStructuralExitConfig` in `trade_management.hpp`):
  - `enabled=true` requires `min_holdtime_ms > 0`.
  - `enabled=true` requires at least one non-disabled rule.
  - Parses `rules[i]` into `ExitRuleSpec{ name, enabled, params }` —
    **does not** interpret per-rule param types.
- **Build time** (`BuildExitPlan` in `structural_exit.hpp`):
  - Converts each `enabled=true` `ExitRuleSpec` into a concrete `IExitRule`.
  - Unknown `name` → throws, listing the known rule names.
  - Missing / mistyped param → throws, naming the rule and the param.
  - Disabled rules are skipped entirely; their params are never read.

This is the same shape as `AppConfig::Load` (parses opaque JSON) →
`StrategyFactory::Build` (interprets names). A disabled rule cannot
produce a bad signal, so a typo inside it cannot become a hidden failure.

## Interaction matrix (nothing gets overridden)

| Existing feature | Effect of structural exit | Where |
|---|---|---|
| **Fixed SL** | Unchanged. Broker enforces it on every tick. Structural close is not needed if SL already fired (position flat → guard resets). | `CheckRisk` / venue bracket |
| **TP** | Unchanged. Two parallel close paths. Whichever fires first wins. Structural closes at market, **not** at TP price. | PaperTrader `CheckRisk`; venue bracket |
| **BE / trailing / scale-out** | Unchanged. Structural runs first in `Update()`. If it closes, BE/trail see `!HasOpenPosition()` next tick. | `PositionManager` |
| **Prop guard** | Unchanged. Close is never vetoed. Prop guard flatten is unaffected. | `PropGuard` / `ObserveProp*` |
| **SinglePositionLock** | Unchanged. After a structural close, `AntiRevengeLock` still gates re-entry. Trigger-based re-entry is permitted but gated by risk modules as today. | `risk_engine.hpp` |
| **`enabled_` on `PositionManager`** | Guard sits inside the master gate. `SetEnabled(false)` disables structural exits too, matching today's intent. Never called today; safe. | `position_manager.hpp` |

### On TP tightness
`tp_r` in `risk` still controls TP as today. If a strategy wants structural
exits to be the *primary* exit, set `tp_r` large enough that TP never wins
the race — noting the ceiling described in "Open items". Nothing in the
code changes; it's a config choice.

## File-by-file changes

### NEW — `include/structural_exit.hpp` (header-only, ~400 lines)
Contents in order:
1. `enum class ExitAction : uint8_t { Hold, Close, Reduce, TightenStop }`
2. `struct ExitSignal { ExitAction action = Hold; double arg = 0.0; const char* reason = ""; }`
3. `struct ExitState { int64_t entry_ts_ms = -1; double mfe = 0.0; double mae = 0.0; }`
4. `class IExitRule` — `Name()`, `OnTrade(ctx,pos,price,ts,ExitState&)`, `OnBook(ctx,pos,ts,ExitState&)`, `Reset()`
5. Six concrete rules (one class per row of the catalogue table)
6. `class ExitPlan` — holds rules, `OnTrade/OnBook/Reset`
7. `class StructuralExitGuard` — owns `ExitState` + `ExitPlan`, exposes `Configure`, `ResetTradeState`, `OnTrade`, `OnBook`
8. `BuildExitPlan(const StructuralExitConfig&)` — factory, throws on unknown name / missing param, skips disabled rules

`StructuralExitConfig` and `ExitRuleSpec` live in `trade_management.hpp`
(see below), not here — they are pure data and must be includable
without dragging in the execution layer.

Guard's close path:
```cpp
broker->ClosePartial(1.0, price, ts, std::string("struct_") + rule->Name());
```

### MODIFIED — `include/trade_management.hpp`
Add the data structs and the resolver:

```cpp
struct ExitRuleSpec {
  std::string    name;
  bool           enabled = true;
  nlohmann::json params;
};

struct StructuralExitConfig {
  bool                      enabled = false;
  int64_t                   min_holdtime_ms = 0;
  std::vector<ExitRuleSpec> rules;
};

StructuralExitConfig ResolveStructuralExitConfig(const nlohmann::json& tm);
```

Resolver behaviour, same idiom as `ResolveLevelPct`:
- Reads `tm["structural_exit"]`. Absent → `enabled=false`, return.
- `enabled=true` requires `min_holdtime_ms > 0`; throws otherwise.
- `enabled=true` requires at least one non-disabled rule; throws
  otherwise (same wording style as "every entry in 'triggers' is
  disabled").
- Parses each `rules[i]` into `{name, enabled, params}` via the same
  `enabled` / `active` / `isActive` spellings `IsDisabled` accepts.
- Does **not** read or validate any other key. No cross-key checks.

### MODIFIED — `include/config_parser.hpp`
Around `config_parser.hpp:56-74` (`TradeManagementConfig` struct), add:
`StructuralExitConfig structural_exit;`
Field defaults off; no existing default changes.

### MODIFIED — `src/config_parser.cpp`
Inside `ParseSection("trade_management", ...)` at ~`src/config_parser.cpp:1152-1199`, **after** the `scale_out` block and before the closing `});`, add:
- Optional read of `tm["structural_exit"]` via `ResolveStructuralExitConfig`.
- Populate `cfg.tm_config.structural_exit`.
No requirement on the block's presence. `Validate` does **not** need new
logic — the resolver throws on bad config, same as `ResolveLevelPct`.

### MODIFIED — `src/factory.cpp`
After the existing `inst.pos_manager->Configure(...)` call at
~`src/factory.cpp:661-668`, add:
`inst.pos_manager->ConfigureStructuralExit(cfg.tm_config.structural_exit);`
Separate call, separate state. No interaction with `Configure`.

### MODIFIED — `src/runner_common.cpp`
After `eng.pos_manager->Configure(...)` at ~`src/runner_common.cpp:175-181`, add:
`eng.pos_manager->ConfigureStructuralExit(ResolveStructuralExitConfig(json_config));`

This is the arming site for grid / WFA / WFA-OOS / replay. Bear in mind
these runners deliberately re-parse the JSON — do not assume
`cfg.tm_config.structural_exit` is populated on those paths.

### MODIFIED — `include/position_manager.hpp`
Five edits:
1. `#include "structural_exit.hpp"`, `#include "market_context.hpp"`.
2. Member `StructuralExitGuard structural_exit_;`.
3. New method
   `void ConfigureStructuralExit(const StructuralExitConfig& c) { structural_exit_.Configure(c); }`.
4. Change `void Update(double, int64_t)` →
   `void Update(const MarketContext&, double price, int64_t ts)`.
   Inside: call `structural_exit_.OnTrade(...)` **before** the existing
   `enabled_` early return; if the position is now flat, return before
   running BE/trail/scale. Add `structural_exit_.ResetTradeState()` on
   the `!HasOpenPosition()` reset block, alongside the existing latches.
5. New method `void UpdateOnL2(const MarketContext& ctx, int64_t ts)`:
   mirror the top of `Update()` (open-position check + reset), then call
   `structural_exit_.OnBook(...)`. **No BE/trail/scale body** — those
   stay trade-path-only. This mirrors how `PositionManager` was already
   designed: BE/trail want a trade price, structural rules want the book.

### MODIFIED — `src/run_loop.cpp`
Two changes.

**L2 catch-up loop** (the `apply_book` lambda, ~`src/run_loop.cpp:38-60`):
after `eng.aggregator->ProcessL2(snap)` and before
`alpha->Evaluate(tick_ctx, false)`, add:
```cpp
if (eng.pos_manager) eng.pos_manager->UpdateOnL2(tick_ctx, snap.timestamp);
```

**Trade handler** (~`src/run_loop.cpp:98-104`): reorder to:
1. `ProcessTrade(live_bar, trade)` — moved up
2. `ProcessTrade(htf_bar, trade)` — moved up
3. Build `MarketContext ctx{...}` once
4. `eng.pos_manager->Update(ctx, trade.price, trade.timestamp)` — gains `ctx`
5. `eng.ptrader->CheckRisk(trade.price, trade.timestamp)` — unchanged
6. `eng.alpha->Evaluate(ctx, candle_finished)` — reuses `ctx`

Reordering is behaviour-neutral: `PositionManager`'s BE/trail branches
read `current_price`, not the bar; `ptrader->CheckRisk` doesn't read the
bar either. `live_bar_` is now current when the guard runs, so delta
rules see the print they're reacting to.

### MODIFIED — `src/live_engine.cpp`
Same two changes.

**L2 case** (`case Event::Type::L2:` ~`src/live_engine.cpp:876-926`):
after `ProcessL2 ×2` and the gap check, **before**
`alpha->Evaluate(tick_ctx, false)`, add:
```cpp
if (engine_.pos_manager) engine_.pos_manager->UpdateOnL2(tick_ctx, ev.l2.timestamp);
```

**TRADE case** (~`src/live_engine.cpp:917-965`): reorder to:
1. `ProcessTrade(live_bar_)` — moved up
2. `ProcessTrade(htf_live_bar_)` — moved up
3. Build `MarketContext ctx{...}` once
4. `pos_manager->Update(ctx, price, ts)` — gains `ctx`
5. `broker->CheckRisk(price, ts)` — unchanged
6. `alpha->Evaluate(ctx, candle_finished)` — reuses `ctx`

Same behaviour-neutral argument as `run_loop.cpp`.

## Implementation order (smallest blast radius first)

1. `trade_management.hpp` — data structs + `ResolveStructuralExitConfig`
   + `config_parser.{hpp,cpp}` parse block. Nothing runs yet; verify
   existing tests pass and a config with `structural_exit` absent still
   loads clean.
2. `include/structural_exit.hpp` — interfaces only, all six rules
   stubbed to return `{}`. `StructuralExitGuard` fully wired, no rules
   active.
3. `position_manager.hpp` — five edits. `Update` signature change
   compiles the runners; `UpdateOnL2` added but no callers yet.
4. `src/run_loop.cpp` + `src/live_engine.cpp` — reorder + context
   threading + `UpdateOnL2` calls. **Critical milestone: end-to-end
   compiles, all existing tests pass, no behavioural change** because no
   rule is active.
5. `factory.cpp` + `runner_common.cpp` — `ConfigureStructuralExit` at
   both arming sites. Still no rules active.
6. Rule bodies, one at a time — `MfeGiveback` → `TimeStop` →
   `ImbalanceBreak` → `DeltaFlip` → `SpoofPull` → `DeltaDivergence`.
   After each: a targeted test in `test/test_position_manager.cpp`.
7. Config schema fixtures in `tests/` — one `enabled=false`, one
   `enabled=true`, one malformed (missing `min_holdtime_ms`), one with a
   disabled rule.

## Tests required

1. **Rule fires, position flat.** `MfeGiveback` with `giveback_r=0.5`
   fires when MFE is 2R and price retraces to 1.5R. Assert:
   `ClosePartial(1.0)` was called, `HasOpenPosition()==false` on the
   next tick, `TradeRecord.exit_reason == "struct_MfeGiveback"`.
2. **`min_holdtime_ms` suppresses on entry candle.** Rule condition met
   at `t=1000ms`, `min_holdtime_ms=5000` — no close. Same rule at
   `t=6000ms` — closes.
3. **`enabled=false` on the block → guard is inert, but state resets.**
   Rule configured, master switch off, `OnTrade` never called;
   `ResetTradeState` still runs on position open/close so a later
   `enabled=true` flips cleanly.
4. **Per-rule disable.** Block `enabled=true`, two rules listed, only
   one `enabled=true`. Assert only the enabled rule's condition closes
   the position; the disabled rule's condition, if triggered, does not.
5. **`enabled=true` with all rules disabled → load throws** with the
   same wording style as the "every trigger is disabled" error.
6. **`UpdateOnL2` does not touch BE/trail.** `SetEnabled(true)`,
   `enable_scale_out=true`, one L2 frame with profit >=
   `scale_out_trigger_r * risk` — assert `ClosePartial` was **not**
   called with `"scale_out"`. Confirms BE/trail stay trade-only.
7. **Unknown rule name in config → load throws** with a message naming
   the rule and listing the known names.
8. **`enabled=true` without `min_holdtime_ms` → load throws.**
9. **Live/backtest parity.** Same trade sequence on `PaperTrader` (via
   `run_loop.cpp`) and via the `BybitBroker::OfflineForTests` harness
   (or a mock `IBroker`) produces the same `struct_*` exit reason and
   the same entry/exit prices (up to slippage — paper slippage is
   deterministic, venue is not, so parity is asserted on the
   **decision**, not the fill price).

## Open items / known edge cases

- **`tp_r: 100` idiom has a ceiling.** `AppConfig::Validate` enforces
  `tp_r * sl_pct < 0.5` when `tp_r > 0`, and `tp_r <= 100`. So
  `tp_r: 100` is valid only when `sl_pct < 0.005`. For a strategy that
  wants structural to be the primary exit on `sl_pct >= 0.005`, the
  config workaround stops working. Two paths out:
  (a) add a `structural_exit.suppress_tp: bool` flag, plumbed through
  `SignalExecutor` (~10 lines across the three `SignalExecutor`
  construction sites) — cleanest;
  (b) document the constraint in the config reference and leave TP as a
  real exit for tight-stop strategies. **Recommendation: ship v1 with
  (b), evaluate on the first live test whether TP actually races
  structural, add (a) only if it does.**

- **Hyperliquid adapter is a stub.** `HyperliquidBroker::ProcessSignal`
  and `ClosePartial` log `"NOT submitting"` — nothing reaches the
  exchange. Structural exits work correctly against the interface, but
  no HL order is sent. This is a pre-existing HL state, not a
  structural-exit concern. When HL signing lands, whoever adds
  `oid`-based resting-order cancellation must also ensure that
  `ClosePartial(1.0)` cancels the resting TP/SL — otherwise the orphan
  bracket fires on the next position.

- **Bybit and BloFin already handle bracket cleanup.** Bybit V5 clears
  position-attached TP/SL when size hits zero, so `ClosePartial(1.0)`
  leaves nothing behind. BloFin explicitly cancels TPSL on close
  (`blofin_broker.cpp:830-834, 907-911, 1047-1049`). No code change.

- **`MfeGiveback` requires `entry_ts_ms` to be set correctly.** On the
  very first tick after an entry, the guard latches `entry_ts_ms` when
  it first sees an open position with `entry_ts_ms == -1`. In practice
  this fires on the same tick the position opens (via `ProcessSignal`
  → next `OnTrade`). If a restart happens with a position already open,
  the guard latches `entry_ts_ms` to *that* tick (i.e. restart time,
  not real entry time) and logs a `LOG_WARN`. Document this in the
  guard's header comment: on venue restart, `min_holdtime_ms` and
  `TimeStop` measure from process start, not trade entry. Acceptable
  for v1.

- **Re-entry on the same tick as a structural close** is possible if
  no risk module gates it. `SinglePositionLock` allows (position is
  flat), `AntiRevengeLock` gates via cooldown. This is identical to the
  SL-hit-then-reenter case today — no new hazard.

## Definition of done

- [ ] Existing test suite passes at every step of the implementation order.
- [ ] All nine scenario tests above pass.
- [ ] A config with `structural_exit` absent loads and behaves
      identically to today (byte-for-byte identical paper-trade output
      on a fixed dataset).
- [ ] A config with `structural_exit.enabled=true` and one active rule
      shows the expected exit reason in `TradeRecord.exit_reason`.
- [ ] A config with `structural_exit.enabled=true` and all rules
      disabled fails to load with a clear error.
- [ ] A config with two rules and one `enabled=false` fires only the
      enabled rule.
- [ ] `GetCurrentPosition().direction == NONE` after a structural close
      (state fully reset, no ghost position).
- [ ] `LOG_WARN` fires if any rule ever returns `Reduce` /
      `TightenStop` (v1 safety net).
- [ ] `src/momentum.md` updated with any deviations from this plan
      before the PR is opened.

## References

- `include/position_manager.hpp:79-83` — `enabled_` semantics; guard
  stays inside the gate.
- `include/ibroker.hpp:52-62, 74` — `GetEquityAt`,
  `GetEquityAtBidAsk`, `ClosePartial` semantics.
- `include/prop_equity.hpp:52-63, 80-82` — canonical flatten via
  `ClosePartial(1.0)`.
- `src/papertrader.cpp:318-321` — `ClosePartial(1.0)` leaves clean
  state.
- `src/bybit_broker.cpp:960-1020` — V5 reduce-only close, no orphan
  bracket.
- `src/blofin_broker.cpp:830-834, 907-911, 1047-1049` — explicit TPSL
  cancel on close.
- `src/hyperliquid_broker.cpp:499-540` — HL `ClosePartial` is a stub;
  note when signing lands.
- `src/factory.cpp:661-668` and `src/runner_common.cpp:175-181` — the
  two `Configure` arming sites.
- `src/run_loop.cpp:98-104` — trade handler reorder site.
- `src/live_engine.cpp:917-965` — mirror of the above, live path.
- `src/factory.cpp:IsDisabled` and `src/config_parser.cpp:381-395` —
  per-component `enabled` / `active` / `isActive` spellings and the
  "every entry is disabled" error that `ResolveStructuralExitConfig`
  mirrors.


