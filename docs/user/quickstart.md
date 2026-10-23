# Quickstart

Commands assume the repository root and a completed Release build.

## Run modes

| invocation | what it does | needs `--config` | credentials |
| ---------- | ------------ | ---------------- | ----------- |
| `./build/mokuren` | interactive TUI: grid search, WFA, single replay | no | no |
| `--live` | real orders on the selected venue | yes | yes |
| `--paper` | real public feed, fills in-process | yes | no |
| `--probe` | startup gate only, print the report, exit | yes | no |
| `--debug` | add to any mode: trace logging, `live_debug.log` | — | — |

`--live`, `--paper` and `--probe` are mutually exclusive and each requires
`--config=<path>`. Any other argument is rejected with exit code 1 instead of
falling through to the TUI.

## 1. Record a dataset

```bash
./build/mokuren_record --symbol BTCUSDT --duration 60
```

Prints the output path (`recordings/bybit_BTCUSDT_<utc>.mkn`) and a run summary.
Ctrl-C stops it cleanly.

## 2. Check the dataset

```bash
./build/mokuren_inspect recordings/bybit_BTCUSDT_<stamp>.mkn
```

Prints record counts, first/last timestamps and timestamp inversions per
channel, and refuses a dataset that is empty, damaged, or whose channels do not
overlap in time. Exit code 0 means a replay can consume it.

## 3. Check the feed without trading

```bash
./build/mokuren --probe --config=/path/to/your.json
```

Connects with the same streamer the live path builds, runs the same startup
gate, prints the per-channel table and exits. Nothing is armed, sized or sent,
and no key is required.

| exit code | meaning |
| --------- | ------- |
| 0 | gate passed |
| 1 | gate failed (the report is printed) |
| 130 | interrupted with Ctrl-C |

## 4. Paper trade

```bash
./build/mokuren --paper --config=/path/to/your.json
```

Real public market data, fills inside the process. The gate runs first; if it
fails the run aborts unless `startup_probe.on_failure` is `warn`.

## 5. Backtest

`./build/mokuren` with no arguments opens the TUI, which drives the three
backtest workflows:

- grid search over every `{"min","max","step","val"}` node in the config,
- walk-forward analysis, which produces out-of-sample configs,
- a single replay of one config.

A replay result carries the 1-minute history, the 15-minute history and the
trade list. Write your first config from the skeleton in
[Configuration](configuration.md).

## Order to learn things in

1. [Configuration](configuration.md) — the file shape and the units.
2. [Components](config-components.md) — what you can put in `triggers`,
   `filters` and `risk.modules`.
3. [Live and paper](live-and-paper.md) — what changes when a process trades.
