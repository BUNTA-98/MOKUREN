# LiveImbalance: mid-candle stacked imbalances

Reference for the changes that let the stacked-imbalance pattern fire while the
candle is still building, plus the performance picture.

## Why

`StackedImbalanceTrigger` reads `history.back()`, so it can only speak after the
candle closes. The pattern (aggressive flow stacking on the footprint diagonal)
is visible much earlier. Missing it cost whole moves.

The footprint was the blocker: `ResetWorkspace` cleared it and only
`PackFootprint` (at flush/close) filled it. Mid-candle it was always empty.

## Approach

Option A: mirror the grid into the live bar as prints arrive. Option B
(book-depth signal like `SpoofHunterTrigger`) was the alternative, but it
reports a different pattern.

---

## Changes

### `include/aggregator.hpp`
- `Bar::UpsertFootprintLevel(const PriceLevel&)` -- insert-or-replace, keeps
  `footprint` price-ordered. Defined in `aggregator.cpp`.
- `Aggregator::PruneStaleFootprint(Bar&)` -- private; drops mirrored levels the
  grid no longer holds.
- `Aggregator::mirror_stale_` -- set when `GetOrAddLevel` slides or re-anchors
  the grid.
- `PackFootprint` comment: it remains the authoritative close-time pack.

### `src/aggregator.cpp`
- `UpsertFootprintLevel`: `std::lower_bound` by price, replace on match, insert
  otherwise. Order is part of the contract -- `ScanFootprint` compares adjacent
  prices, so appending would neighbour levels that do not neighbour in price.
- `PruneStaleFootprint`: `remove_if` keeping only levels whose grid index is
  in range AND whose `live_workspace[idx].price` still equals the mirrored
  price. Cross-checks the grid so a re-anchor (whole grid wiped) is handled.
- `GetOrAddLevel`: sets `mirror_stale_ = true` before the slide/re-anchor.
- `UpdateBarData`: order is
  1. `GetOrAddLevel` (may set `mirror_stale_`)
  2. `PruneStaleFootprint` if flagged
  3. apply the trade to the grid level
  4. `bar.UpsertFootprintLevel(*level_ptr)`

Why this order: the prune must run before the next insert so dropped levels
never sit next to live ones; the upsert must run after the trade is applied so
the mirror reflects post-trade state. `GetOrAddLevel` has no `Bar&` parameter,
hence the deferred flag.

`PackFootprint` still rebuilds from the grid at close, so the live mirror is a
read view for open candles -- it cannot leak into history.

### `include/trigger.hpp`
- `namespace imbalance`: the shared scan, extracted verbatim from
  `StackedImbalanceTrigger`.
  - `StackRun` -- `count`, `lo`, `hi`, `worst_ratio`, `Reset()`.
  - `Scan` -- `best_buy`, `best_sell`, plus `levels`, `lo`, `hi`, `bid_sum`,
    `ask_sum` (diagnostics for the trace line).
  - `ResolveTick(const Bar&)` -- tick inference, bar-tick first, adjacent-delta
    fallback.
  - `ScanFootprint(...)` -- the whole run scan.
- `StackedImbalanceTrigger` now calls both. No behavioural change.
- New `LiveImbalanceTrigger` (below).

Why shared: two triggers report the same pattern. A second copy of the scan
would eventually disagree about what a stack is, and the live trigger would
silently become a different strategy.

---

## `LiveImbalanceTrigger`

Edge-triggered, tick-level, reports the same pattern earlier.

- Reads `context.live_bar.footprint` each tick; the close-time trigger reads
  `history.back()`.
- Same `ScanFootprint`, so the two cannot drift.
- Ticket shape matches the rest of the codebase: `entry_price = bar.close`,
  `stop_loss = 0`, `take_profit = 0`. Funnel applies `risk.sl_pct` / `risk.tp_r`
  via `SignalExecutor`. Hardcoding a stop would re-create the un-sweepable
  `sl_pct` problem documented on `StackedImbalanceTrigger`.

State:

- `buy_latched_` / `sell_latched_` -- set on first qualifying print per side;
  cleared only when that side's run drops below `min_stacked_count_`. A
  break-and-reform is a new setup.
- `latched_bar_ts_` -- reset latches on a new `timestamp_start`. Without it, a
  stack late in candle N would suppress the first stack in N+1.
- `both_warned_` -- `EvaluateTick` runs per print; the both-sided rejection
  would otherwise log on every print. Throttled to once per candle. The reject
  itself is unchanged.

Guards:

- `bar.close <= 0.0` -- a ticket at price 0 is not a setup.
- `footprint.size() < 2` -- nothing to compare.
- `EvaluateCandle` returns `{}` -- close-time reporting belongs to
  `StackedImbalanceTrigger`; two reports would be two tickets for one setup.

Deliberate omission: no `require_candle_agreement`. Mid-candle the body is
unfinished, so "close > open" says nothing about where the imbalance sat.

---

## Wiring caveats

1. `AND_Trigger` does not override `EvaluateTick` -- it inherits
   `ITrigger::EvaluateTick` which returns `{}`. A `LiveImbalanceTrigger` inside
   an `AND_Trigger` will never fire. Use standalone or inside `OR_Trigger`, or
   add the override.
2. `min_level_qty` default `0.0001` is BTC-scale ("not zero"). Set it per
   instrument, like `StackedImbalanceTrigger`.

---

## Performance

### Aggregator: slower, but small

Added per print in `UpdateBarData`:

- one branch on `mirror_stale_` (near-always not taken)
- `UpsertFootprintLevel`: one `lower_bound` (log n) + 24-byte copy on the
  common path (existing price); O(n) shift only on new prices, clustered at
  candle open
- `PruneStaleFootprint` only on a grid slide, O(n) once

No new allocations per print as long as `footprint` stays under high-water
capacity (`PackFootprint` reserves at close and ResetBar keeps capacity). No
extra memory in history -- `PackFootprint` re-packs at close. No per-print
logging.

Estimate: **+20-50 ns per print** vs existing OHLC/grid work. A few percent
at most on the trade path. **Not worth optimizing.**

### Trigger: where the time went

`LiveImbalanceTrigger::EvaluateTick` runs `ScanFootprint` **on every print**:
O(n) over the footprint, **two passes**:

- main loop: n-1 adjacent-pair comparisons, up to two divisions on qualifying
  branches
- aggregate pass: `lo` / `hi` / `bid_sum` / `ask_sum` over every level --
  **only used by `LOG_TRACE`**

Compare: `StackedImbalanceTrigger::EvaluateTick` is free; `SpoofHunterTrigger::EvaluateTick`
is a ring walk that breaks early (a handful of iterations).

Rough estimates, n ≈ 100:

| Component                  | Added cost    |
|----------------------------|---------------|
| `UpsertFootprintLevel`     | ~20-40 ns     |
| `ScanFootprint` (2 passes) | ~150-400 ns   |
| **per print total**        | **~200-450 ns** |

Scaling:

- 1k prints/s   -> ~0.05% of a core. Invisible.
- 100k prints/s -> ~2-5% of a core. Fine.
- 1M prints/s   -> ~20-45% of a core. Worth attention.
- Backtest replay: ~200-450 ns extra per record; ~2-4 s per 10M records.

**Verdict: the trigger scan dominates the new cost. The aggregator is noise
next to it.**

### Bottleneck ranking

1. `imbalance::ScanFootprint` per print -- largest added cost, O(n), two passes
2. `Bar::UpsertFootprintLevel` -- `lower_bound` + copy; O(n) insert on new
   prices
3. `PackFootprint` at close -- now redundant (mirror ⊆ grid at close) but runs
   once per candle; not a bottleneck
4. `PruneStaleFootprint` -- O(n) per grid slide, rare

### Optimizations, in order of leverage

1. Fold the aggregate pass into the main loop -- accumulates each iteration
   sees `footprint[i-1]`; one extra read for `back()`. Halves memory traffic,
   no semantic change.
2. Make aggregates conditional (a `bool diagnostics` parameter).
   `LiveImbalanceTrigger` can skip them; `StackedImbalanceTrigger` fires once
   per candle, so it keeps them.
3. Early exit on `count >= min_stacked_count` -- cheapest win, but loses
   `worst_ratio` / band from the diagnostic line.
4. Incremental scan -- a change at level `i` only affects pairs `(i-1,i)` and
   `(i,i+1)`. O(log runs) per print but real complexity; only if profiling
   shows it dominates.

**Recommendation: leave both as-is until a profiling run says otherwise.**
Option 1 is the only clear steal and is a small behaviour-neutral refactor.

---

## Verifying

Add:

- a `ProcessTrade` throughput bench (loop a recording, records/s before vs
  after)
- a `LiveImbalanceTrigger::EvaluateTick` bench at representative footprint
  sizes

The numbers above are estimates. Actual ratio depends on the footprint
distribution in your recordings, and non-trigger work in the replay loop (JSON,
logging, I/O) may already dominate enough that neither change is visible
end-to-end.
