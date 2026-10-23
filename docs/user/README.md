# Mokuren — User Manual

Mokuren is a C++17 engine for researching, backtesting and forward-testing
order-flow strategies on crypto perpetual futures. It ingests trade and
order-book data, builds candles and footprints, evaluates triggers through
filters and risk modules, and can trade a simulated paper account or a venue
account. It also records the live feed into a replayable dataset.

This manual is about using the program. Internals are documented separately for
maintainers.

## Reading order

| # | Page | What it covers |
| - | ---- | -------------- |
| 1 | [Installation](installation.md) | dependencies, build, tests, binaries, runtime paths |
| 2 | [Quickstart](quickstart.md) | the run modes, first recording, first paper run |
| 3 | [Data](data.md) | `.mkn` recordings, historical directories, timestamps, ordering |
| 4 | [Configuration](configuration.md) | file anatomy, units, precedence, full skeleton, valid ranges |
| 5 | [Engine settings](config-engine.md) | venue, environment, execution, risk, trade management, prop, startup probe |
| 6 | [Components](config-components.md) | every trigger, filter and risk module with its parameters |
| 7 | [Config examples](config-examples.md) | complete paper configs and one grid config |
| 8 | [Backtesting](backtesting.md) | grid search, walk-forward, replay, what a run reports |
| 9 | [The TUI](tui.md) | pages, the config editor, the scan workflow |
| 10 | [Live and paper](live-and-paper.md) | startup gate, credentials, shutdown, logs |
| 11 | [Recording and tools](recording-and-tools.md) | `mokuren_record`, `mokuren_inspect` |
| 12 | [Running as a daemon](deployment.md) | the systemd template, a pre-flight checklist, restart behaviour |
| 13 | [Telegram notifications](telegram.md) | bot setup, chat id, the event schema, delivery quirks |
| 14 | [Troubleshooting](troubleshooting.md) | error messages and known quirks |

A new config is written by copying the skeleton in
[Configuration](configuration.md), then filling in the blocks from
[Engine settings](config-engine.md) and picking components from
[Components](config-components.md). [Config examples](config-examples.md) has
three complete files to start from.

## Mental model

- One JSON config describes one run. Configs are per-user files; the repository
  ships none. You choose the path with `--config=`.
- The venue (`exchange`) selects the market-data streamer, the broker and the
  integrity profile. They are built from the same venue spec, so they cannot
  disagree about environment, host, category or symbol.
- A run is a pipeline: data → candles/footprint → triggers (per candle and per
  tick) → filters → risk modules → sizing → execution → management.
- Before a live run trades, a startup gate checks that every required channel
  is delivering well-formed frames. The same connection is then used, so a
  passing gate cannot be a different handshake from the one that trades.
- Numeric risk and fee settings are **fractions**: `0.002` is 0.2%, `0.0004` is
  0.04%.
- Recordings are the preferred input for backtests. A recording is produced by
  the same parser the live path uses.

## Run modes at a glance

| invocation | purpose |
| ---------- | ------- |
| `mokuren` | interactive TUI: grid search, walk-forward, single replay |
| `mokuren --live --config=…` | real orders on the selected venue |
| `mokuren --paper --config=…` | live public feed, simulated fills |
| `mokuren --probe --config=…` | run the startup gate only, print the report, exit |
| `mokuren --debug` | add to any mode: trace logging + `live_debug.log` |

The three mode flags are mutually exclusive, each requires `--config=`, and any
argument outside this set is rejected with exit code 1 rather than falling
through to the TUI.
