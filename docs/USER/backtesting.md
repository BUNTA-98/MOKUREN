# Backtesting

Three workflows, all driven from the TUI. None of them contacts a venue: the
factory is built with `live_mode=false`, so the process has no streamer and no
execution adapter.

## Choosing a dataset

The scanner reads the profile selected on the CONFIG page (`[1]`), walks it, and
uses the last string found under a `filepath`, `data_path` or `dataset` key —
wherever it sits in the document. Both a `.mkn` recording and a historical
directory work (see [Data](data.md)).

A profile with none of those keys falls back to nothing: the scan stops with
`ERR: no dataset in <path>`. The literal `data` directory and the
`environment.dataset` fallback exist only for a caller that passes no path at
all (CLI, tests).

## Grid search

Every object carrying numeric `min`, `max` and `step` is a sweep axis:

```json
"sl_pct": {"min": 0.001, "max": 0.004, "step": 0.001, "val": 0.002}
```

- Values are `min + k*step`, k = 0 … `floor((max-min)/step)`. The count is
  computed rather than accumulated, so the last value is tested exactly.
- `val` is what a live or paper run uses; the sweep ignores it.
- No grid node means one run — the config as written.
- A component with `enabled: false` is not swept.
- `min > max`, a negative `step` or a non-finite bound aborts with
  `grid error: invalid range for parameter '<name>'`. `step: 0` does **not**
  abort: it is read as `1.0`, so a typo becomes a sweep of unit steps.
- Two nodes sharing a leaf name (`trigger_r` under both `break_even` and
  `trailing`) are reported under their full JSON path, so neither hides the
  other.

The leaderboard ranks runs by net profit and shows, per row: rank, net profit,
the `tp` / `be` / `sl` percentages, max drawdown, Sharpe, Sortino, a
Monte-Carlo drawdown estimate and the trade count.

`tp` / `be` / `sl` are outcome shares — winning, break-even and losing trades as
a percentage of the trade log — not the config's take-profit and stop values.
The underlying result also carries the parameter map, the full config, the trade
log and — for a walk-forward run — the out-of-sample configs.

A run that hit a prop limit sets `prop_breached`. The leaderboard does not show
it, so a breached permutation is ranked like any other; `results.csv` carries
the flag.

A permutation with zero trades is dropped unless it also breached a prop limit.
The leaderboard and `results.csv` can therefore hold fewer rows than the sweep
had permutations, and a swept value that never traded leaves no trace at all.

## Run artifacts

A grid search creates `runs/scan_<YYYYmmdd_HHMMSS>/` relative to the working
directory. A walk-forward run writes no artifacts at all: its per-window configs
travel inside the result so the replay can use them.

| file | contents |
| ---- | -------- |
| `used_config.json` | the base config as loaded, grid nodes intact |
| `results.csv` | one row per kept run: the swept parameters in report order, then `NetProfit,MaxDrawdown,Trades,WinRate,Sharpe,PropBreached` |

The directory is created even when every run throws, so an empty
`results.csv` means "nothing kept", not "the scan never started".

## Walk-forward analysis

The dataset span is cut into windows from the first to the last timestamp:

```
in_sample  = [start,           start + is  - 1]
out_sample = [start + is,      start + is + oos - 1]
next start = start + step
```

A window is kept only when its out-of-sample end lies inside the data, so the
tail is dropped rather than truncated. The three spans come from the profile's
`backtest` block:

| key | default | meaning |
| --- | ------- | ------- |
| `wfa_in_sample_days` | `14` | in-sample length per window |
| `wfa_out_of_sample_days` | `7` | out-of-sample length per window |
| `wfa_step_days` | `7` | how far the window advances |
| `wfa_mode` | `false` | copied into a walk-forward result's config; never read |

`wfa_mode` does not select the workflow. `S` runs a grid search and `W` runs a
walk-forward no matter what the file says.

Each window is optimised in-sample and then evaluated out-of-sample. The winner
is the in-sample permutation with the highest net profit; its out-of-sample run
contributes the trades that end up in the stitched result.

- A window whose grid produces no permutations is skipped, and the progress
  counter still advances.
- A permutation that throws inside a worker is logged and the window carries on
  with the rest.
- The aggregate result is emitted only if the stitched out-of-sample run has at
  least one trade, or if some window breached a prop limit. A breach is reported
  rather than filtered out: an untradeable config is still a finding.
- `prop_breached` on the aggregate is set if **any** window's out-of-sample run
  breached. The in-sample flag is logged per window and not carried forward.
- The result's `full_config` is the *last* window's winning config with
  `backtest.wfa_mode` patched to true. Use `wfa_configs` to replay the
  aggregate; `full_config` is a single window, not the whole run.
- `WfaRunner::ReplayWFA` stitches the out-of-sample segments into one trade list
  and one bar history, but the TUI never calls it. Selecting a walk-forward row
  and pressing ENTER replays the *last window's* winning config through the
  single-run path, so the trade log shows one window while the row's numbers
  describe the whole run.

## Single replay

Replays one config against one dataset and hands the result to the trade log,
the equity curve and the footprint inspector.

- Trades and book are read from the **same** path, trades first, book second.
- A book frame strictly older than the current print is applied before it; a
  frame sharing the print's millisecond is applied after, matching the live
  stream's "print before book" ordering.
- Book frames after the last print are still applied, so a book-only signal in
  the tail is not lost.
- An open position is closed at the end of the session at the last print (or
  the last closed bar if the final bar carries no prints), and stamped with the
  last applied timestamp rather than with the final bar's open.
- A prop-enabled run that would have breached a limit sets `prop_breached` on
  the result.

A replay has no side effects on the account and never writes a recording.

## Gotchas

**A replay returns nothing after a config change.** Check `logs/engine.log`. A
damaged recording, an empty channel and a config without a dataset all produce
an empty result, and the reason exists only in the log.

**Every window looks short.** Run `mokuren_inspect`; inverted timestamps make
the slice helpers stop early.

**A regenerated CSV is ignored.** Delete the `.bin` cache beside it.

**A sweep reports a strategy that never ran.** The grid scanner and the factory
accept slightly different spellings of "disabled". Use `false` or `0`.

**A grid result does not reproduce in a replay.** Grid search and walk-forward
apply a book frame stamped at the same millisecond as the print *before* the
print; a single replay applies it *after*. Same dataset, different fill context,
so any result that depends on book timing moves between the two.

**Candle-only runs never fire a tick trigger.** A replay without book frames
logs `no L2 snapshots for dataset …` and every order-flow trigger evaluates as
if the book were empty.

**A walk-forward row and its replay disagree.** The row aggregates every
out-of-sample window; ENTER replays the last window alone. Compare the row's
trade count with the log before drawing conclusions.
