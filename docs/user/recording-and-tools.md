# Recording and tools

## `mokuren_record`

```
mokuren_record [--symbol NAME] [--out PATH] [--venue NAME] [--env NAME]
               [--category NAME] [--duration SECONDS] [--l2-interval MS]
               [--no-validate] [-h|--help]
```

| flag | default | meaning |
| ---- | ------- | ------- |
| `--symbol` | `BTCUSDT` | venue symbol to subscribe to |
| `--out` | `recordings/<venue>_<symbol>_<UTC>.mkn` | output file |
| `--venue` | `bybit` | session metadata; only `bybit` has a streamer |
| `--env` | `mainnet` | session metadata |
| `--category` | `linear` | session metadata |
| `--duration` | `0` | stop after N seconds; `0` = until Ctrl-C |
| `--l2-interval` | `0` | at most one book frame per N ms; `0` = every frame |
| `--no-validate` | off | write frames the live gate would reject |

The recorder takes no config file: it has no strategy. Validation is structural
only — one level per side is the minimum a frame needs to be a book at all —
because what the triggers need on top of that is a strategy decision.

Trades are never downsampled. A missing print changes the footprint; a coarser
book mostly does not. Use `--l2-interval 100` when a full-rate book makes the
file unmanageably large.

Missing parent directories are created. The recorder attaches to the streamer's
data callbacks but not its lifecycle callbacks: the process owns stream end and
stream error, so a recording cannot silently end itself and leave an unreported
hole.

### Summary

| counter | meaning |
| ------- | ------- |
| `trades seen / written / rejected` | validation outcome per print |
| `l2 seen / written / rejected` | validation outcome per book frame |
| `l2 throttled` | dropped by `--l2-interval` |
| `l2 gaps` | streamer sent a book-gap sentinel; no frame exists to record |

Ctrl-C sets a flag, the streamer is stopped (joining its producer thread), and
only then is the file finalised. If finalisation fails the file is truncated and
the process exits non-zero.

Exit codes: `0` success; `1` bad arguments, unknown venue, cannot create a
directory, cannot open the file, or cannot finalise it.

## `mokuren_inspect`

```
mokuren_inspect <dataset>
```

`<dataset>` is a `.mkn` recording or a historical directory. The tool goes
through `DataManager`, i.e. the same two calls a replay makes, so a clean result
means the backtest can consume the data — not merely that the bytes decode.

It prints:

- record counts and first/last timestamps per channel (UTC and raw),
- timestamp inversions per channel,
- the alignment verdict between the two channels,
- a refusal if the dataset is damaged, empty, or misaligned.

Exit codes: `0` usable; `1` refused, empty, misaligned, or bad usage. Its log
goes to `logs/inspect.log`, with console output on so a skipped block or a
checksum failure is visible rather than swallowed.

## Other scripts in `tools/`

`FETCH.py`, `L2CONV.py`, `HEATMAP.py` and `3DPLOT.py` are research and
conversion helpers; they are not covered by this manual.

`TELEGRAM.py` is the event notifier and has its own page:
[Telegram notifications](telegram.md). It needs `redis`, `requests` and
`python-dotenv`; `tools/requirements.txt` pins the broader Python environment.

## Related

- [Data](data.md) — what these tools produce and consume.
- [Live and paper](live-and-paper.md) — the gate `--no-validate` bypasses.
