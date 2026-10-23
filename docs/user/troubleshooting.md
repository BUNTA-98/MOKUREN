# Troubleshooting

## Reading the error

Most failures are one line in `logs/engine.log` plus a message on stderr. The
logger is rooted at the working directory, so "the log is empty" usually means
"started from a different directory". The first log line records the argv of the
run, which is how two sessions sharing a log file are told apart.

## Startup and configuration

**`unknown argument '…'`**
The flag is not in the closed set (`--live`, `--paper`, `--probe`,
`--config=<path>`, `--debug`). This is deliberate: an ignored typo used to fall
through to the TUI.

**`--live/--paper/--probe requires --config=<path>`**
A mode flag was given without a config. The bare TUI is the only mode that needs
none.

**`no market-data streamer for exchange '…'`**
The `exchange` value has no streamer in this build. Only `bybit` has both a
streamer and an execution adapter.

**`no execution adapter for exchange 'hyperliquid'`**
Expected. `hyperliquid` supports `--paper` and `--probe`, not `--live`.

**Mainnet is refused / orders go to testnet**
Set `allow_mainnet: true` explicitly. Testnet is the default so a typo cannot
spend real money.

**`config error: ...`**
A required key is missing or mistyped. The message names the full path
(`risk.sl_pct`). There is no fallback for the venue, for take-profit, or for any
trade-management level. Fix the file rather than adding a default elsewhere.

**`AppConfig validation failed: ...`**
The value is present but out of range. See the valid-ranges table in
[Configuration](configuration.md).

**`--live` complains about missing credentials, `--paper` does not**
Intended. Paper and probe load with `require_venue_credentials = false`.

**A config key has no effect**
Unknown keys are ignored by the loader. Compare the file against the skeleton in
[Configuration](configuration.md); a misspelling falls back to a default or
fails on the required sibling instead.

## Startup gate

**`refusing to trade on a feed that failed its startup checks`**
The gate ran and did not pass. Run `--probe --config=…` to get the same report
without touching the engine. Check that the venue is reachable, that the symbol
and category are right, and that the message-rate thresholds fit the venue.

**`not enough sustained data after … ms`**
That is the rate check: the message prints each required channel's delivered
count, its observed per-minute rate and the configured minimum. A healthy but
slow hour reads identically to a broken feed, so compare the observed rate with
the minimum before blaming the venue.

**The probe fails before any frames arrive**
The venue's profile declares fewer book levels than `startup_probe.min_l2_depth`.
Lower the requirement, or use a venue that publishes the depth your triggers
read.

**The probe fails on a single bad frame**
One fatal frame on a required channel ends the soak immediately instead of
waiting for the deadline. The report's `reason` names the integrity code that
tripped.

**The probe passes but the strategy cannot see depth**
The default preset checks depth 2. `SpoofHunter` reads five levels per side, so
set `min_l2_depth: 5` for those configs. `StackedImbalance.levels` is a stack
count, not a book depth.

**`startup_probe.on_failure=warn`**
That line means the run started *without* the checks, over a second connection.
It is an explicit override, not a normal path.

## Data

**`mokuren_inspect` refuses the dataset**
Either a block checksum failed or a channel is empty. The refusal is the point:
`DataManager` does not return a short channel as if it were clean.

**Replay finds no trades**
Check `logs/engine.log`. A replay that fails and a replay with no signals both
return an empty result, so the log is the only distinction — a failed one logs
`replay single run: <reason>` (or `replay wfa: <reason>`). Also check that the
config names a dataset: without it, the replay falls back to the literal
directory `data`.

**Every WFA window looks short**
Run `mokuren_inspect` and look at the inversion count. The slice helpers stop at
the first record past the window end and assume ascending timestamps.

**Timestamps look shifted**
Timestamps are normalised by magnitude (ns / µs / ms). If the span is still
wrong by a factor of 1000, the source is mixing units.

**A recording is empty**
The feed produced no usable frames. Check the symbol, the venue status and the
`trades rejected` / `l2 rejected` counters. `--no-validate` will show whether
validation or the feed is at fault.

**A CSV dataset loads zero trades**
The reader needs a timestamp, price, quantity and either a maker or a taker-side
column, and it drops every row at or below 2017-07. A header it does not
recognise produces an empty vector without an error. Compare the header against
the layouts in [Data](data.md).

**A CSV dataset loads an empty book**
The L2 conversion refuses a file that maps to zero price levels, and the loader
then returns an empty book rather than throwing. `mokuren_inspect` reports
`l2: 0` and the run continues book-blind.

**Regenerated a CSV and nothing changed**
The `.bin` cache beside it is read in preference to the CSV and is never
invalidated by a change to the source. Delete `trades_cache_v2.bin` /
`L2_cache_v3.bin` from the dataset directory.

## Strategy behaviour

**Filter results differ between machines**
`TimeOfDayFilter` converts the bar timestamp with `localtime_r`, so its verdict
depends on the host timezone. Run backtests with `TZ=UTC` if you rely on it.

**`OrderbookImbalanceFilter` lets a trade through with no book**
It returns `true` when the relevant side's quantity is zero or negative. It is
fail-open, unlike most other filters; do not rely on it to prove a book exists.

**SpoofHunter never fires**
It needs a five-level aggregate above `min_wall_qty` (default 30) in the
lookback, and the book at the current frame to collapse. The defaults are sized
for BTC and a deep book; a shallow feed will not reach them.

**StackedImbalance finds no stacks**
The trigger reads `bar.tick_size`. If it is zero it infers a tick from the
footprint, which is a fallback, not a configuration. Set `tick_size` in the
config.

**A trigger inside an AND never fires**
`AND_Trigger` implements candle consensus only; tick evaluation of an AND
returns nothing. Tick-only triggers belong in an OR or alone. Note that the
factory always builds an OR, so an AND is not reachable from a config anyway.

**Live mode puts on more than one position**
`risk.modules` is empty or lacks `SinglePositionLock`. Nothing is added
implicitly. `MaxDrawdownLock` exists in the engine but cannot be selected from a
config, and `risk.max_daily_loss` only reaches the paper account.

**A live run ignores signals for a few seconds after arming**
The venue-path risk modules read the exchange's cached account state. Until the
first private websocket frame arrives the balance reads as 0, so
`MaxLeverageLock` rejects everything and the sizer produces no volume.

## Process

**No output at all**
The TUI was selected. Backtest, live, paper and probe all log to
`logs/engine.log`; use `--debug` for trace detail.

## Exit codes

| code | meaning |
| ---- | ------- |
| 0 | normal completion, or a probe that passed |
| 1 | fatal error, failed probe, bad arguments |
| 130 | interrupted probe (128 + SIGINT) |
