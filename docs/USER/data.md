# Data

## Two dataset kinds

The replay path accepts exactly two inputs:

1. a `.mkn` recording written by `mokuren_record`, and
2. a historical directory containing `trades.csv` and `L2.csv`.

Both go through `DataManager::LoadAllTrades` and
`DataManager::LoadAllL2Snapshots`. `mokuren_inspect` calls those same two
functions, which is why it is the tool to run when a replay behaves oddly.

## The `.mkn` format

The normative specification is `docs/market_data_format.md`. What matters when
using it:

- A 128-byte file header: magic, format and minimum reader version, endian tag,
  sizes, metadata offset/size and CRC, block-index offset/size, file size,
  flags.
- Independent blocks, each with a 56-byte header: magic, kind, codec, record
  stride, record count, first/last timestamp, payload offset/size and a
  CRC-32C.
- A block index of 32-byte entries at the end, plus a header patch on close.

Record kinds are `Trade` (1), `BookSnapshot` (2), `BookDelta` (3); `Mark` (100)
and `SessionMarker` (101) are reserved.

A metadata block (JSON) carries venue, environment, category and symbol. The
reader uses the venue to select an integrity profile, so a wrongly labelled
recording yields a reader configured for the wrong venue.

### Units

Every timestamp on disk is nanoseconds since the Unix epoch. The engine works in
milliseconds and shifts once at the reader boundary. Do not mix the two in
tooling.

Ingest also normalises by magnitude, for sources that disagree:

| magnitude | interpreted as |
| --------- | -------------- |
| ≥ 1e17 | nanoseconds |
| ≥ 1e14 | microseconds |
| otherwise | milliseconds |

If a dataset still looks shifted by a factor of 1000, inspect the span rather
than the individual rows.

### Depth and aggressor

`kMaxBookDepth` is 20 levels per side. A dataset may declare fewer; it may not
declare more without a format version bump. Top-of-book-only sources write
depth 1, which is a valid frame and therefore passes the gate.

Trades store `Aggressor::{Unknown,Buy,Sell}`, never a venue's "buyer is maker"
boolean. Footprint, delta, POC and every imbalance threshold depend on this; a
flipped flag inverts all of them.

Snapshot flags carry `resync`, `truncated` and `stale`. A `l2 gaps` counter in a
recording summary means the streamer sent a book-gap sentinel — the book was
unknown at that moment, and there is no frame to store.

## CSV directories and caches

A historical directory is decoded once and then cached next to the CSV. The
cache is what a later run actually reads.

| source | cache file | written when |
| ------ | ---------- | ------------ |
| `trades.csv` | `trades_cache_v2.bin` | the cache is missing or does not hold a whole number of records |
| `L2.csv` | `L2_cache_v3.bin` | the cache is missing, or truncated, or rebuilt after a failed conversion |

Three consequences:

- **A changed CSV is not picked up.** Nothing compares the cache against the
  CSV's mtime or size. Delete the `.bin` (or the whole directory) after
  regenerating a CSV, otherwise the old rows are replayed again.
- **Cache names carry a version.** `v2`/`v3` exist because the record layouts
  changed (a `TradeEvent` normalised to milliseconds, an `L2Snapshot` widened
  from 200 to 688 bytes). A leftover cache from an older build is simply not
  found, so it is safe to keep old files around — but do not rename one to
  match a newer version.
- **A truncated cache self-heals.** A partial trailing record makes
  `LoadAllL2Snapshots` delete the file and reconvert from the CSV in the same
  run.

### What the CSV readers accept

`trades.csv` needs a timestamp, a price, a quantity, and either a maker flag or
a taker-side column. Headerless files are sniffed with the canonical Binance
Vision `aggTrades` layout, so a file that starts with data works. Bybit
(`Buy`/`Sell`) and Hyperliquid (`B`/`A`) side tokens are mapped to the
aggressor; a taker sell means the buyer was the maker.

`L2.csv` needs a timestamp and at least one bid and one ask price/quantity
pair. Exports with more levels than the engine stores are truncated to the
maximum depth; a recognised file that maps to zero levels is refused rather
than cached as all-zero snapshots.

Rows with a normalised timestamp **at or below 2017-07** are dropped, in both
readers, to filter out header junk and unit-mangled values. That is a silent
floor: a legitimately older dataset loads as empty.

## Ordering

`SliceTrades` and `SliceL2` stop scanning at the first record past the window
end, so they are only correct on an ascending vector. An inverted pair makes
every backtest window quietly short instead of visibly wrong.

Neither loader sorts. The `.mkn` reader returns records in stored order (the
writer only appends, so they should be ascending) and the CSV path returns rows
in file order. Run `mokuren_inspect` instead of assuming either: it counts
inversions per channel.

The two channels must also line up in time, but the tolerance is generous: the
start timestamps and the end timestamps may be up to **60 minutes** apart. A
larger gap is refused, because one channel would run blind for the whole
session.

## Live feed ordering

A streamer emits trades and book updates as one strictly ordered stream by
exchange timestamp. On a tie the trade comes first. Triggers rely on this to
decide whether the book changed before or after a print.

## Damaged recordings

The reader counts failed payload checksums, unknown block kinds and skipped
records. `DataManager` refuses a recording that carries an `error`, or that
skipped blocks, trades or book records — the book case is spelled out in the
message because a run with a lost book is otherwise indistinguishable from a
strategy that found no setups.

Warnings alone do not refuse the file. An unknown record kind is forward
compatibility (a newer recorder may add marks this build does not know) and a
metadata checksum mismatch costs session labels, not records. Both are logged
and the run proceeds.

The replay loads trades first and the book second, from the same path, so a
large recording is decoded twice. That is the behaviour being measured, not an
artifact of the tool.
