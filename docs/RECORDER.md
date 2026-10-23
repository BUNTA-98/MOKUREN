# Recording and inspecting market data

Two small binaries sit next to `mokuren`:

| binary | purpose |
| --- | --- |
| `mokuren_record` | records a live public feed to one `.mkn` file |
| `mokuren_inspect` | reads a dataset the way the backtest does and prints what came back |

`mokuren_record` does **not** trade. It attaches to the same venue streamer the
engine uses, so the bytes it writes were produced by the parser that trades --
not by a second, less exercised websocket client.

---

## Quick start

```bash
cmake --build build -j --target mokuren_record mokuren_inspect

# 60 seconds of BTCUSDT
./build/mokuren_record --symbol BTCUSDT --duration 60

# what did we get?
./build/mokuren_inspect recordings/bybit_BTCUSDT_20261023T015800Z.mkn
```

Point a backtest config at the file and the engine reads it instead of a CSV
directory:

```json
{ "environment": { "dataset": "recordings/bybit_BTCUSDT_20261023T015800Z.mkn" } }
```

Both `DataManager::LoadAllTrades` and `DataManager::LoadAllL2Snapshots` branch
on the file magic, so every caller that resolves `environment.dataset` gets the
recording with no further changes.

---

## mokuren_record

```
usage: mokuren_record [options]

  --symbol NAME        venue symbol (default BTCUSDT)
  --out PATH           output .mkn file
                       (default recordings/<venue>_<symbol>_<utc>.mkn)
  --venue NAME         session metadata only (default bybit)
  --env NAME           session metadata only (default mainnet)
  --category NAME      session metadata only (default linear)
  --duration SECONDS   stop after N seconds (0 = until ctrl-c)
  --l2-interval MS     at most one book frame per MS (0 = every frame)
  --no-validate        write frames the live gate would reject
  -h, --help           this text
```

`--venue` only accepts `bybit` today. The check is deliberate: the venue name
goes into the file metadata, and a bybit feed written under a `binance` label
would pick the wrong venue profile when the file is read back.

`--env` and `--category` are **metadata only** -- they do not change the
endpoint. They exist so a mainnet and a testnet session are distinguishable
later.

### What gets written

One `.mkn` file, snapshot-only (`book_encoding: "snapshot"`,
`granularity: "mbp"`), depth 20 per side, little-endian. Trades and book frames
interleave in the order they arrived, and both channels come back sorted by
timestamp.

Approximate cost per record:

| record | bytes |
| --- | --- |
| trade print | 48 |
| book frame (20 levels/side) | 680 |

The book dominates: at 40 frames/s that is ~1.6 MB/min, ~98 MB/hour. Prints add
little.

### Stopping it

Any of:

- `ctrl-c` / `SIGTERM` -- the normal way.
- `--duration N` -- the recorder stops itself, then finalises.
- the feed reporting a natural end or an unrecoverable error.

Shutdown is ordered: the streamer is stopped (which joins its callback thread)
*before* the file is finalised. Recording into a file that is still being
produced from another thread would race the header patch.

If the process is killed with `SIGKILL`, the file keeps every block written so
far but loses its metadata and index trailer. That is a readable file, not a
corrupt one -- see *Unfinalised recordings* below.

### Reading the summary

```
--- recording summary ---
file:              recordings/bybit_BTCUSDT_20261023T015800Z.mkn
trades  seen:      1264
trades  written:   1264
trades  rejected:  0
l2      seen:      811
l2      written:   811
l2      rejected:  0
l2      throttled: 0
l2      gaps:      0
```

- **rejected** -- frames the live validator refused (non-finite price,
  non-positive timestamp, one-sided book, depth below `min_book_depth`). These
  are *not* written: the format has no field meaning "this record was junk", so
  writing one would make it indistinguishable from real data.
- **throttled** -- book frames skipped because of `--l2-interval`. Expected, not
  an error.
- **gaps** -- reconnect sentinels from the streamer (zeroed snapshots). Counted,
  never written: a gap is not a book.

A healthy run has `rejected: 0`. `rejected` climbing while `written` stays put
means the feed is producing malformed frames, which is worth investigating
before trusting the recording.

Note that `l2 written: 0` with a large `l2 rejected` count is the classic
symptom of a validator/format disagreement rather than a quiet market -- the
file will look fine and contain no book at all.

If nothing at all was written, the recorder says so on stderr:

```
nothing was written: the feed produced no usable frames
(check the symbol and the venue status)
```

That message does **not** change the exit code. Read the summary; do not rely
on `$?` alone.

### Exit codes

| code | meaning |
| --- | --- |
| 0 | file finalised |
| 1 | bad arguments, unknown venue, could not open the path, or the file could not be finalised (truncated) |

---

## mokuren_inspect

```
usage: mokuren_inspect <dataset>
```

`<dataset>` is either a `.mkn` recording or a historical directory
(`trades.csv` / `L2.csv`, cached as `.bin` beside them).

It calls the same two functions the replay calls -- `DataManager::LoadAllTrades`
and `DataManager::LoadAllL2Snapshots` -- so it measures the path the backtest
actually takes, not the decoder in isolation. A file can decode perfectly and
still be unusable in a replay (wrong units, an empty channel, a descending
vector the slicing helpers assume cannot happen); this is what catches that.

```
dataset: recordings/bybit_BTCUSDT_20261023T015800Z.mkn
trades: 1264
  first: 2026-10-23T01:58:26Z (1792720706058)
  last:  2026-10-23T01:58:44Z (1792720724088)
  inversions: 0
l2: 811
  first: 2026-10-23T01:58:24Z (1792720704995)
  last:  2026-10-23T01:58:44Z (1792720724374)
  inversions: 0
```

What to look at:

- **counts** -- both channels should be non-zero. A book-only or trade-only
  recording is legal but half-blind, and the replay will warn about the missing
  channel rather than fail.
- **inversions** -- must be 0. `SliceTrades` / `SliceL2` break out of their loop
  once they pass `end_time`, which is only correct on an ascending vector. An
  inversion means every walk-forward window is quietly short.
- **first/last** -- the channels should overlap. It is normal for book frames to
  start before the first print (the book is pushed continuously) and to end
  after the last one; the replay drains that tail.

The alignment check (`ValidateDataAlignment`) runs only when *both* channels are
non-empty, and tolerates up to 60 minutes of skew at each end. Consequently a
single-channel dataset exits 0 -- check the counts, not just the exit code.

Warnings (truncated file, skipped block, checksum mismatch, dropped records) go
through the logger: printed to the console and also written to
`logs/inspect.log`.

### Exit codes

| code | meaning |
| --- | --- |
| 0 | something loaded; channels overlap (or only one exists) |
| 1 | bad usage, nothing loaded, or the two channels do not overlap in time |

---

## Using a recording in the backtest

```bash
./build/mokuren --config configs/grid_btc.json
```

with

```json
{
  "environment": { "dataset": "recordings/bybit_BTCUSDT_20261023T015800Z.mkn" },
  "backtest": { "...": "..." }
}
```

Everything downstream is unchanged: grid search, WFA and single replay all read
the same path, and `.mkn` files carry both channels in one file, so there is no
side cache to keep in sync.

Two properties of the replay are worth knowing because they interact with how a
recording was captured:

**Ring-buffer window.** The aggregator keeps the last 1024 book frames. At full
rate that is roughly 26 seconds of history; the depth triggers can only look
back that far. Recording with `--l2-interval 100` does two useful things at
once: the file shrinks, and the effective wall-clock window grows five-fold.

**Memory during replay.** The decoded book is held in a `std::vector<L2Snapshot>`
for the whole run -- 688 bytes per frame, about 99 MB/hour at 40 frames/s. This
is a loading cost, not a leak; it is also the reason `--l2-interval` is worth
using for long sessions.

---

## Operational notes

**Unfinalised recordings.** A `SIGKILL`ed recorder leaves valid blocks with no
metadata trailer. `mokuren_inspect` reads it and warns that the session fields
are unknown; `DataManager` returns the records and logs the warning. Nothing is
lost except the descriptive metadata and the index.

**Corrupted blocks.** Each block carries a CRC-32C. A block that fails its
checksum is skipped with a warning and the read continues at the next block,
because block boundaries are explicit. A damaged *size* field desynchronises the
stream and stops the read with an error -- everything before that point is
still returned.

**One recorder per path.** The writer is deliberately not thread safe: one
producer, one closer. Two processes pointed at the same output file will
interleave and produce garbage.

**Same machine class.** The format is little-endian by definition. The writer
refuses to run on a big-endian host rather than emit a file that reads back as
nonsense.

**Do not mix with `tools/L2CONV.py`.** That script writes the old 200-byte
`L2.bin` layout (5 levels, `#pragma pack`) while the current `L2Snapshot` is 688
bytes with 20 levels. The engine reads neither that layout nor that filename.
Recordings bypass the CSV/cache pipeline entirely; historical CSV directories
still go through `L2_cache_v3.bin`.

**Validation is on by default.** `--no-validate` writes frames the live gate
would have rejected. It exists to study a bad feed, not to build a dataset: a
recording made that way can contain frames the engine would never have acted
on.

---

## Troubleshooting

| symptom | likely cause |
| --- | --- |
| `l2 written: 0`, `l2 rejected` large | validator and recorder disagree about depth or one-sidedness |
| `l2 written: 0`, `l2 seen: 0` | wrong symbol, or the venue refused the subscription (check the log for `request REJECTED`) |
| `trades written: 0`, `l2 written: 0` | nothing usable arrived; check the symbol and venue status |
| `l2 seen` is large, `l2 written` moderate | `--l2-interval` is set; expected |
| `inspect` prints `l2: 0` for a recording that had frames | the dataset branch was not taken; confirm the path is a single file, not a directory |
| `inspect` reports an `ERR: DATA DESYNC` | the two channels are more than an hour apart; the recording is probably a concatenation of two sessions |
| replay warns `no L2 snapshots` | the dataset has prints but no book -- tick triggers will not fire |

---

## Format

The byte layout lives in `include/market_data/format.hpp`; `writer.hpp` and
`reader.hpp` both include that header rather than keeping their own copy of the
constants, so the two cannot drift. Useful facts for anyone scripting around
the files:

- 128-byte file header, 56-byte block header, 32-byte index entry.
- Timestamps on disk are nanoseconds; the engine works in milliseconds and the
  reader converts once, at the boundary.
- Aggressorship is stored as an enum (`Buy` / `Sell`), never as a venue's
  "is buyer maker" boolean -- that flag flips sign between venues and would
  invert footprint and delta on import.
