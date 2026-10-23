# Live and paper trading

## Startup sequence

1. The config file is read and parsed as JSON.
2. Grid objects (`{"min","max","step","val"}`) are collapsed to their `val`, so
   the factory only ever sees plain numbers.
3. `AppConfig::Load` validates the file. `--live` requires API credentials;
   `--paper` and `--probe` do not.
4. `StrategyFactory::Build` constructs the pipeline from the configured venue.
   For `--paper` the streamer is real but no execution adapter is built, so
   every fill goes to the in-process paper account.
5. The orchestrator connects and runs the startup gate.
6. On success the engine is armed on the same connection that was just
   validated. On failure it is not.

## Credentials and environment

- `--live` on bybit needs `api_key` and `api_secret`.
- Mainnet orders need an explicit `allow_mainnet: true`; the default is
  testnet, so a missing or misspelled key cannot route real orders.
- `--paper` and `--probe` never sign a request, so they load without
  credentials.
- The venue must have a streamer in this build. Only `bybit` has both a
  streamer and an execution adapter; `hyperliquid` has a streamer only, so a
  `--live` hyperliquid run aborts with `no execution adapter for exchange
  'hyperliquid'` and `--paper` is the supported rehearsal.

## The startup gate

Before arming, every channel the venue profile marks as required must deliver
well-formed frames for the configured soak, at the configured message rates and
book depth. The checks run in this order and the first failure ends the probe:

1. **Capability.** If the venue's profile declares fewer book levels than
   `startup_probe.min_l2_depth`, the run is refused before a single frame is
   examined. This is the check a shallow feed cannot slip past: a depth-1
   snapshot is structurally valid, so a count-based gate would pass it while the
   depth triggers read zeroed level-2 slots.
2. **Fatal frames.** One fatally malformed frame on a required channel fails the
   probe immediately. The feed is not quiet, it is lying, and waiting for the
   deadline would only add latency to the same verdict.
3. **Reconnects.** More drop-and-recover events than `max_reconnects` fails the
   soak even if the counts are eventually met.
4. **Sustained rate and soak.** Each required channel must have delivered at
   least `ceil(rate_per_min × elapsed_minutes)` frames — with a floor of one
   whenever the rate is positive — *and* the soak must have run for at least
   `min_duration_ms`.
5. **Deadline.** At `timeout_ms`, partial success is failure.

Consequences worth knowing:

- The "required" figure in the report is recomputed from elapsed time on every
  evaluation, so it grows during the soak. It is a moving target, not a count
  tuned for one market hour.
- The rate is cumulative from the moment the probe started, not a sliding
  window. An early burst can carry a channel through a later silence, and an
  early silence costs frames a later burst has to make up for.
- The default preset is `development` (a short soak); `production` is the
  longer one. Explicit keys override either, so you can keep a long soak and
  lower the frame rates for a thin instrument.
- `min_l2_depth` must match what the strategy actually indexes. `SpoofHunter`
  always aggregates five levels per side (not configurable), so set
  `min_l2_depth: 5` when it is in use. `StackedImbalance.levels` is a stack
  count, not a book depth. The default checks depth 2.
- A pass prints a per-channel table and the run continues on the connection
  that was just validated.
- A failure prints the report and aborts the run by default. Setting
  `startup_probe.on_failure` to `warn` reconnects without enforcing the gate;
  this is a debugging escape hatch, it is logged as a warning, and it is never
  the default.

## Checking a feed without trading

```bash
./build/mokuren --probe --config=/path/to/your.json
```

`--probe` runs the same startup checks `--live` runs, over the same streamer,
and prints the per-channel report. It is built with the paper broker and loads
without credentials, so no request is signed and no order can leave the
process. Exit codes:

| code | meaning |
| ---- | ------- |
| 0 | gate passed |
| 1 | gate failed (the report is printed) |
| 130 | interrupted with Ctrl-C |

## What ends a session

- Ctrl-C: graceful shutdown, the orchestrator is stopped and the dropped-event
  count is printed.
- The streamer reporting end of input: logged as `market stream ended`.
- A stream error: logged with the error text, and the run ends.

The live loop polls the orchestrator, so a dropped socket or a closed stream is
not mistaken for a quiet market.

## Paper account

`--paper` fills in-process. The starting equity is the config's
`initial_balance`, so drawdown is measured against a defined start rather than
against the first observation — which arrives after the first fill and would
understate the day's start.

The prop guard keeps its own base and does not inherit that value in the
backtest paths; see [Engine settings](config-engine.md) for how the two can
disagree.

## Venue path: cold start

On the venue path the risk modules and the position sizer read the exchange's
cached account state, which stays empty until the first private websocket frame
arrives. Until then the balance reads as 0, `MaxLeverageLock` rejects every
signal and no size is produced. A live run that appears to ignore signals for a
moment after arming is usually this, not a broken trigger.

## What suppresses entries

A live run can be healthy and still not trade. These gates run before your own
`risk.modules`, and each one logs when it fires:

- **Prop guard.** The warning state refuses new entries while open trades stay
  managed. A breach flattens once, with the exit reason `prop_guard`, and stays
  halted for the rest of the day or process.
- **Book gap.** A frame with no bid or no ask is the streamer saying the local
  book is no longer faithful (reconnect, transport error). Entries stop until
  the next real snapshot; the log then says `book recovered; entries re-enabled`.
- **Book degraded.** The event queue overflowed (65,536 frames), so frames were
  dropped. This latch never clears: the run keeps managing the open position but
  stops opening new ones for the rest of the process.
- **Venue account state.** See the cold-start note above.

All of them apply to entries only. Stops, targets and trade management keep
running, because those already have a position to protect.

## Shutdown

Stopping the run flushes the in-progress candle and force-closes any open
position at the last known price. The paper path uses the full backtest
accounting; the venue path sends a reduce-only close stamped `shutdown`. The
closing line reports the queue-overflow count and the number of frames the
integrity layer refused — two different numbers, and the second is usually what
explains a book that does not match the venue's own tape.

## Logs

`logs/engine.log` records the exact invocation (`argv`), gate verdicts, risk
rejections (trace level) and shutdown. Console logging is on for `--live`,
`--paper` and `--probe`, and off for the TUI. `--debug` raises the level to
trace and enables `live_debug.log`.
