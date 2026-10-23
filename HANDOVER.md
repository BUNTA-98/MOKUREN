# HANDOVER

MOKUREN // market-data intake. `.mkn` is now a first-class backtest dataset.
Status: compiles, refuses damaged recordings, dataset path is no longer dropped.
Below is what still bites. No new features before these.

---

## 1. ORDERS

Run these before touching code. They produce the evidence the rest of this
document is guessing at.

- **Record.** `mokuren_record --symbol BTCUSDT --out recordings/btc_1h.mkn --l2-interval 500`
  Duration >= 1h. Under an hour gives too few candles to judge anything.
- **Inspect.** `mokuren_inspect recordings/btc_1h.mkn`
  Non-zero exit = the recording is refused. Read the reason, not the count.
- **Shadow.** Run a config with `--paper` against the live venue.
  Any divergence between paper and a replay of the same recording is a defect.
  Record it. Do not "fix" it by editing the strategy.

Recorder limits, known:
- bybit only.
- `min_book_depth = 1`. Shallow books are written. Depth triggers will read
  zeroed levels.
- Snapshot-only. `WriteDelta` is never called.

---

## 2. DEFECTS

Ordered by damage. All confirmed in source; none speculative.

### D1 — Tie ordering differs between scan and replay
`grid_runner.cpp` / `wfa_runner.cpp` advance book frames with
`l2.timestamp <= trade.timestamp`. `replay_manager.hpp` uses strict `<`.
Same recording, two orderings. Replay cannot reproduce the scan that selected
the config. Pick one. `<` is correct per `live_streamer.hpp`.

### D2 — Worker exceptions are swallowed
Grid and WFA catch per-permutation throws and only `LOG_ERROR`. The run
disappears from `results.csv` and the leaderboard. A config error is
indistinguishable from "strategy found no setups". Count them and fail the scan.

### D3 — Scan/WFA omit the replay's end-of-session handling
No drain of trailing book frames. Close uses `live_bar.close`, which is `0.0`
when the final bar has no prints — books a total loss. `ReplayManager` has
fallbacks for both; the scan paths do not.

### D4 — `tools/L2CONV.py` writes the dead layout
Emits 200-byte records, 5 levels. `L2Snapshot` is 688 bytes, 20 levels. The
resulting cache is misread at the new stride. Either rewrite it or delete it.

### D5 — Writer metadata is false; index is dead weight
`book_encoding` is hardcoded `"snapshot"` and `granularity` `"mbp"` regardless
of content. `index_offset` / `index_size` are written and never read —
`DecodeBlocks` walks linearly, and every run decodes the file **twice**
(trades loader + book loader). First performance wall on long recordings.

### D6 — Scanner path sniffer takes the last match
`ui_scanner.hpp` / `ui_manager.hpp` overwrite `data_path` on every hit of
`filepath` / `data_path` / `dataset`, in sorted key order. A nested key
silently wins. One key, top level, or stop the sniffer at the first match.

### D7 — Empty book is only a warning
A trade-only recording passes validation by design. No `require_l2` key
exists. Acceptable today, untenable once a book-driven strategy is selected.

---

## 3. AFTER THE FIXES

Re-record. Inspect. Replay. Only then trust a scan.

Nothing in this document is a strategy opinion. It is intake integrity.
