# Recording and tools

## `mokuren_record`

```
mokuren_record [--symbol NAME] [--out PATH] [--venue NAME] [--env NAME]
               [--category NAME] [--duration SECONDS] [--l2-interval MS]
               [--daily] [--overwrite] [--no-validate] [-h|--help]
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
| `--daily` | off | `--out` is a root; one file per UTC day |
| `--overwrite` | off | daily mode: replace the existing file on a collision |

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

### Daily mode

With `--daily`, `--out` is a directory root rather than a file. The recorder
writes one file per UTC day:

    <root>/<venue>-<symbol>/<venue>-<symbol>-YYYY-MM-DD.mkn

The day is `ts_exchange_ns / kNsPerDay`, so boundaries are UTC midnight. The
file is opened lazily on the first record that passes validation — a stream
that never ticks leaves no empty header behind.

Rotation happens between records, never inside one: a block is closed at a
complete boundary, so every closed file ends on a valid index entry. A late
or out-of-order record stays in the current file rather than being dropped;
one out-of-order print inside a day is a lesser evil than losing a record.

Files are opened as `.part` and renamed to `.mkn` on close. A crash leaves a
`.part`; the merger (`mokuren_merge`, below) skips it by design rather than
repairing it.

The recorder only ever writes full `BookSnapshot` records (never
`BookDelta`), so no book state crosses a file boundary. A reader that
rebuilds from the first snapshot it sees ends up in the same state it would
have in the daily file — which is why the merger can concatenate payloads
verbatim.

On a restart that lands on a day whose file already exists, the new session
is written to `<base>T<HHMMSS>.mkn` (UTC time of the first record), and
further collisions on the same day get `-1`, `-2`, … Both sessions coexist;
nothing is clobbered. `--overwrite` removes the collision instead — use it
when the old file is known stale.

### Summary

| counter | meaning |
| ------- | ------- |
| `trades seen / written / rejected` | validation outcome per print |
| `l2 seen / written / rejected` | validation outcome per book frame |
| `l2 throttled` | dropped by `--l2-interval` |
| `l2 gaps` | streamer sent a book-gap sentinel; no frame exists to record |
| `rotations` | daily mode: UTC-midnight cuts performed |

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

## `mokuren_merge`

```
mokuren_merge --input DIR --output ROOT --venue V --symbol S
              [--kind weekly|monthly] [--overwrite]
```

Merges a flat directory of daily `.mkn` files into weekly or monthly files.
The scan is non-recursive: point `--input` at one instrument's directory
(`<root>/<venue>-<symbol>`), not at the root above it. Getting this wrong
yields `scanned=0` and exit `0` — a silent no-op, not an error.

The merger only understands the layout `mokuren_record --daily` produces. A
single-file (non-daily) recording is not an input it is built for.

| flag | default | meaning |
| ---- | ------- | ------- |
| `--input` | — | directory to scan (one instrument) |
| `--output` | — | root for merged files; output lands in `<root>/<venue>-<symbol>/` — use a root separate from the recording root |
| `--venue` | — | instrument the merged files belong to |
| `--symbol` | — | instrument the merged files belong to |
| `--kind` | `weekly` | window size: `weekly` or `monthly` |
| `--overwrite` | off | replace an existing output file |

Files are ordered by each input's index `first_ts_ns`, never by filename —
the recorder's `T<HHMMSS>` restart suffix is harmless. The window a record
lands in is a pure function of its timestamp, using the same day key the
recorder uses for filenames:

- `weekly`: `day_key / 7` — epoch-aligned (Thursday–Wednesday), *not* ISO
  weeks. The label is the first day of the window (`YYYY-MM-DD`), which is
  always a Thursday: `bybit-BTCUSDT-2025-01-16.mkn` covers Jan 16–22.
- `monthly`: `YYYY * 100 + MM`. The label is `YYYY-MM`.

A window is decided purely by each file's `first_ts_ns`. A straggler record
the recorder kept in the *previous* day's file lands in the *previous*
window — the file is the unit, not the record.

Payload blocks are copied verbatim through `Writer::AppendBlock`; records
are never re-serialised. Every source block's CRC is re-verified before it
is appended. A damaged source block is a hard error, not a skip — a hole in
the merged file would look exactly like a venue gap in the reconstruction.

Skipped (counted, not fatal):

- `*.part` files — a recorder that crashed,
- files whose header says they were never finalised,
- files with zero records.

Every input file's metadata must match `--venue`/`--symbol`; a directory
holding two instruments is a hard error, because the merged file would be
labelled as one and silently contain both.

Output is written as `<venue>-<symbol>-<label>.mkn.part` and renamed to
`.mkn` on success. A failed merge leaves any existing `.mkn` untouched and
removes the in-progress `.part`.

Re-running against the same `--output` without `--overwrite` fails the second
time ("output exists"). With `--overwrite` the content is idempotent: the same
inputs produce the same bytes. A window that is still being recorded is a
snapshot of what existed at run time — rerun with `--overwrite` after each new
day lands, or wait until the window closes.

Read the stats line, not just the exit code:

    scanned=… skipped=… merged=… windows=… records=…

- `skipped > 0` is normal: it counts `.part` files, unfinalised (crash)
  files, and zero-record files. Confirm the count matches what you expect.
- `windows=0` with exit `0` means nothing grouped — almost always a wrong
  `--input` path, or every file was a `.part`.
- `records=` is the sum of each source's index count; a low number means a
  source was unfinalised.

The merged file's metadata advertises `depth_per_side` at the writer default
(20), not whatever the source recorded at. Decoding is unaffected — readers
use each block's own `record_size` — but do not trust that field on merged
files as a statement about the source.

Exit codes: `0` success; `1` merge failed (bad input, damaged block, rename
failure); `2` bad usage.

## Other scripts in `tools/`

`FETCH.py`, `L2CONV.py`, `HEATMAP.py` and `3DPLOT.py` are research and
conversion helpers; they are not covered by this manual.

`TELEGRAM.py` is the event notifier and has its own page:
[Telegram notifications](telegram.md). It needs `redis`, `requests` and
`python-dotenv`; `tools/requirements.txt` pins the broader Python environment.

## Related

- [Data](data.md) — what these tools produce and consume.
- [Live and paper](live-and-paper.md) — the gate `--no-validate` bypasses.
