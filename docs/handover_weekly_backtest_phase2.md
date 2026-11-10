# Handover: weekly backtest — remaining work

The streaming path is landed, tested and measured. The book is no longer
materialised per permutation: a 7-permutation grid sweep of an 8.4 GB recording
(5.36 M trades) peaked at **1.28 GiB**, flat with respect to file size, and the
log confirmed `grid search: 5357039 trades, book streamed, 8 threads`.

Both test files pass: `test/test_session_cursor.cpp` (cursor equivalence) and
`test/test_run_loop.cpp` (book tail drain, forced-close timestamp, end-price
fallback). `ReplayManager`'s streaming overload now warns when a recording
carries no book, matching the vector overload.

Nothing below blocks a commit of the current state.

## 1. The 1.28 GiB floor (measure, do not guess)

Flat is not small: 1.28 GiB is ~4x the "tens of MB per worker" the design
predicted, and it does not scale with the file, so it is a fixed per-run cost.
Trades account for 171 MB (5.36 M x 32 B); the rest is unexplained.

The reorder deque is ruled out: a deque pinned at `kMaxReorderEntries` would
have emitted anyway and warned once, and the log carries no such warning.

Decisive experiment — rerun the same sweep with ONE worker:

- flat between 1 and 8 workers → a fixed per-cursor cost (index buffer, block
  payload buffer, a moderately sized deque)
- ~8x → per-worker data, i.e. something copies the trades vector

Do not write a fix before that number exists.

## 2. Sweep speed: the page cache does not hold the file

Each permutation re-streams the whole book channel. The measured run read
7 x 8.4 GB = ~58.8 GB logically and pulled ~12.4 GB off the device (~79 % hit
rate). The 8.4 GB recording cannot be resident in 7.6 GiB of RAM, so the
working set permanently evicts and ~21 % of every sweep is disk.

Consequences:

- **Do not do Phase 3 (index seek) for speed.** The per-permutation index read
  is ~19 MB against 58.8 GB of payload reads.
- **Phase 5 (grid partition) only helps when the grid sweeps an aggregation
  axis** (`interval_ms`, `macro_interval_ms`, `tick_size`, `vwap_reset_hour`),
  i.e. when `A > 1`. A pure strategy sweep has `A == 1` and pays one decode per
  permutation by construction; making that faster needs **one decode, many
  strategies**, which is a different restructure and is not designed yet.

If a sweep ever feels too slow, the order is: (a) the one-decode-many-strategies
restructure, (b) Phase 5 partitioning. Not Phase 3.

## 3. Optional, small

- **`SweepResult`.** `TestResult` keeps `trade_log` (each `TradeRecord` holds a
  `std::vector<StopEvent>`) plus a full `nlohmann::json full_config` per
  permutation, and `UIResult::top_results` retains them for the process
  lifetime. ~350 MB at 1000 permutations — real, ~40x smaller than the book,
  never the freeze. Do it when already editing the type.
- **Reject instrument constants as sweep axes.** `environment.tick_size` and
  `lot_step` are not parameters; a swept value silently misprices every run in
  the grid. `GridScanner::FindParams` recurses the whole JSON tree with no key
  whitelist, so this belongs in `AppConfig`'s validation.

## 4. Known gaps in the current tests

- **End-price fallback: only the first branch is exercised.** With any print in
  the run `FlushLastCandle` leaves a nonzero `live_bar.close`, so `last_price`
  and `history.back().close` are unreachable from a fixture. The test pins the
  invariant (a forced close never prices at 0.0), not the branch.
- **`mokuren_inspect --day YYYY-MM-DD` has still never decoded a real day.**
  The earlier "argument syntax failure" was a usage error — the flag was typed
  with no value — not a bug. One real run verifies the range path
  (`DataManager::LoadAllRange` → `ReadSessionFileRange`).

## 5. Verification

```
cmake -S . -B build && cmake --build build -j
./build/engine_tests "*cursor*,*book frames*,*forced close*"
```

One real day through the range path:

```
./build/mokuren_inspect data/2026/merged/bybit-BTCUSDT/merge1.mkn --day 2026-11-10
```

RSS measurement — start the sweep from the grid page, not the command line:

```
/usr/bin/time -v ./build/mokuren
```

Read `Maximum resident set size`, and the grid log line: `book streamed` is the
streaming branch, `book N snapshots` means `IsSessionRecording` returned false
and `VectorL2Feed` won.

## 6. Gotchas

- `main()` accepts only `--live`, `--paper`, `--probe`, `--config=<path>` and
  `--debug`. A bare config path is rejected as an unknown argument; no
  arguments opens the TUI, and the grid scan is started from the grid page.
- `mokuren_inspect`, `mokuren_record` and `mokuren_merge` are separate
  binaries. There is no `mokuren inspect` subcommand.
- `SessionCursor::Open` reads and sorts the whole index per call (~19 MB for a
  week, per permutation per worker). Not worth fixing for speed (see 2), but it
  is why a large sweep pays a fixed cost per permutation.
