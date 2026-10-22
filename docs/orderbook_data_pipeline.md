# Order Book & Market-Data Ingestion — Reference

Status: **final**. Verified against source on 2026-10-22.

Sources read: `include/trade_event.hpp`, `include/aggregator.hpp`,
`include/market_context.hpp`, `include/trigger.hpp`, `include/filter.hpp`,
`include/factory.hpp`, `include/strategy.hpp`, `include/alpha_engine.hpp`,
`include/live_engine.hpp`, `include/csv_parser.hpp`,
`include/timestamp_utils.hpp`, `include/market_data/format.hpp`,
`include/market_data/book_view.hpp`, `include/bybit_streamer.hpp`,
`include/hyperliquid_streamer.hpp`, `include/binance_streamer.hpp`,
`src/aggregator.cpp`, `src/main.cpp`, `src/factory.cpp`, `src/live_engine.cpp`,
`src/bybit_streamer.cpp`, `src/hyperliquid_streamer.cpp`,
`src/binance_streamer.cpp`, `src/csv_parser.cpp`, `test/test_filters.cpp`,
`CMakeLists.txt`.

Every implementation file this document refers to has now been read. The only
remaining unverified area is the `.mkn` on-disk format, which has no reader or
writer in the tree (§16).

---

## 0. Direct answers

**Does SpoofHunter use L1 or L2?**
L2. It sums `bids[0..5)` and `asks[0..5)`. It never reads
`best_bid_*`/`best_ask_*`.

**How many levels?**
It asks for **5** (`depth_levels_ = 5`, private, no constructor parameter, no
setter). It receives 1 on Bybit and Binance, up to 20 on Hyperliquid. The
engine's ceiling is 20; nothing reads that deep.

**Is the engine configured for the first 20 levels?**
No. 20 is a compile-time capacity (`kMaxBookDepth`). There is no depth config
key anywhere: `StrategyFactory::MakeTrigger` passes `lookback_ms`,
`min_wall_qty` and `drop_threshold` and nothing else. `include/factory.hpp`
states that every component param is optional with a silent default, so even a
misspelled `depth_levels` key is ignored rather than rejected.

**How do the streamers translate?**
Independently. Each begins from a `memset`-zeroed `L2Snapshot`, fills what its
venue sends, mirrors `best_*` into `bids[0]`/`asks[0]`, stamps a millisecond
timestamp, and hands it to `Aggregator::ProcessL2`. There is no shared
normaliser and no validation layer.

**Do CSVs parse correctly?**
Trades: yes — timestamp normalisation, aggressor mapping and header detection
are all correct. Books: **no**. `CSVLoader` has no L2 row processor, nothing
calls `SniffL2Columns`, and `CsvColumns` carries no timestamp index for books.

**Does Hyperliquid give us L2?**
Yes. `HyperliquidDataStreamer` subscribes `l2Book`, a full snapshot per push,
up to 20 levels per side, best-first, `time` already in milliseconds.

**What does that mean in practice?**
`min_wall_qty = 30.0` compares against the **sum of five slots**. On Hyperliquid
that is a five-level aggregate. On Bybit and Binance four of those slots are
structurally zero, so it becomes "the single best bid/ask level holds
≥ 30 BTC". Same class, same config, two different strategies.

---

## 1. Pipeline at a glance

```
venue ws ──┐
.mkn file ─┼─► L2Snapshot ─► Aggregator::ProcessL2 ─► L2RingBuffer (1024)
csv       ─┘                                             │
                                                         ├─► GetLatestL2()
                                                         │        │
                                                         │        ▼
                                                         │   MarketContext.latest_l2
                                                         │        │
                                                         │        ▼
                                                         │   SpoofHunterTrigger (L2, top-5)
                                                         │   OrderbookImbalanceFilter (L1)
                                                         │        │
                                                         │        ▼
                                                         │   SignalExecutor
                                                         └─► Bar::last_l2 (written, never read)

venue ws ──► TradeEvent ──► ProcessTrade ──► footprint ─► StackedImbalance
                                                        DeltaAbsorption
```

Two independent halves. The book feeds exactly one trigger and one filter; the
footprint feeds the two candle triggers.

---

## 2. `L2Snapshot` — the in-memory book

`alignas(16)`, no `#pragma pack`, size pinned by `static_assert`s.

| Offset | Size | Field |
|-------:|-----:|-------|
| 0   | 8   | `int64_t timestamp` (ms) |
| 8   | 8   | `double best_bid_price` |
| 16  | 8   | `double best_bid_qty` |
| 24  | 8   | `double best_ask_price` |
| 32  | 8   | `double best_ask_qty` |
| 40  | 8   | padding — aligns `bids` to 16 |
| 48  | 320 | `L2Level bids[20]` |
| 368 | 320 | `L2Level asks[20]` |
|     | **688** | **total** |

`L2Level` is `alignas(16) { double price; double qty; }` — 16 bytes, so every
level access is naturally aligned.

The struct comment records that the previous layout change bumped the L2 cache
filename to v3 and that `L2_cache_v2.bin` is incompatible. Any change to field
order, depth or padding requires another bump; the asserts make silent breakage
impossible.

### 2.1 What it does not carry

| Missing | Consequence |
|---------|-------------|
| `n_bids` / `n_asks` | A 1-level book and a 20-level book are indistinguishable. Bybit and Binance are the 1-level case on **every** snapshot. |
| `seq` | Gaps and reordering cannot be detected. Bybit sends `u`/`seq`; Hyperliquid pushes whole snapshots. Neither is stored. |
| `flags` | No stale / truncated / resync signal reaches a consumer. |
| `ts_recv` | No latency measurement. |

Every one of these exists in the on-disk format (§12) and in `BookView` (§6.3).
They are discarded at the adapter boundary.

### 2.2 Three stale "200-byte" comments

All predate the 688-byte layout and would mislead anyone sizing memory or
writing a new adapter from them:

- `LiveOrchestrator::Event` in `include/live_engine.hpp` — *"a full 200-byte
  L2Snapshot"*
- `MarketContext::latest_l2` in `include/market_context.hpp` — *"avoids copying
  the 200-byte snapshot"*
- `Aggregator::GetLatestL2` in `include/aggregator.hpp` — *"no second 200-byte
  copy kept around"*

---

## 3. Depth budget

| Stage | Depth | Evidence |
|-------|------:|----------|
| Bybit subscription | **1** | `"orderbook.1." + symbol_` |
| Hyperliquid subscription | up to 20 | `l2Book`, capped at `MAX_L2_DEPTH` |
| Binance subscription | **1** | `btcusdt@bookTicker` in the header default |
| `.mkn` record | up to 20, per-record count | `kMaxBookDepth`, `BookSnapshotRec` |
| `L2Snapshot` arrays | 20 fixed, count lost | `static_assert(sizeof == 688)` |
| `SpoofHunterTrigger` read | **5** | `depth_levels_ = 5` |
| `OrderbookImbalanceFilter` read | **1** | `best_bid_qty` / `best_ask_qty` |
| depth config key | **none exists** | `include/factory.hpp`, `src/factory.cpp` |
| footprint grid (unrelated) | 5000 buckets | `MAX_GRID_LEVELS` |

`include/factory.hpp` is the authoritative config surface and says so
explicitly: *"Every param below is OPTIONAL and falls back to the shown
default: a missing or misspelled key never fails, it silently takes the
default."*

---

## 4. Ring buffer and lookback arithmetic

```cpp
constexpr size_t L2_RING_BUFFER_SIZE = 1024;   // 1024 * 688 = 704,512 B
idx = (head + L2_RING_BUFFER_SIZE - 1 - age_index) % L2_RING_BUFFER_SIZE;
```

- `get_history(0)` is the **newest** snapshot and returns `nullptr` for
  `age >= count`. SpoofHunter's walk direction is correct.
- The buffer is a value member of `Aggregator`. The live worker feeds **both**
  aggregators, so a live run holds two buffers (~1.34 MiB) plus two footprint
  workspaces (`5000 × 24 B ≈ 117 KiB` each).
- `GetLatestL2()` returns `get_history(0)`, or a `static` zero `L2Snapshot`
  whose `timestamp == 0`. That is exactly what SpoofHunter's first guard tests.

**Lookback reach.** With `C = 1024` samples arriving at `R` per second, the
reachable lookback is `C / R` seconds, so 500 ms needs `R <= 2048`.

| Snapshot rate | Samples in 500 ms | reachable? |
|---------------|------------------:|------------|
| 10 /s | 5 | yes |
| 100 /s | 50 | yes |
| 1000 /s | 500 | yes |
| 2048 /s | 1024 | exactly at the limit |
| 5000 /s | 2500 | **no — permanently silent** |

Hyperliquid pushes on every book change and can exceed this in bursts. The
failure mode is silent: the oldest resident snapshot is newer than the
threshold, `past_l2` stays null, the trigger returns `NONE` forever.

**Lookback quantisation.** The test is `>=` against a discrete stream, so the
effective lookback is `ceil(lookback / sample_period) * sample_period`. At a
200 ms feed, 500 ms becomes 600 ms.

---

## 5. `SpoofHunterTrigger`

```cpp
SpoofHunterTrigger(int lookback_ms = 500,
                   double min_wall_qty = 30.0,
                   double drop_threshold = 0.9);
int depth_levels_ = 5;   // private, no ctor arg, no setter
```

Config surface from `MakeTrigger`, cross-checked against `include/factory.hpp`:

| key | arg | default |
|-----|-----|--------:|
| `lookback_ms` | `lookback_ms` | 500 |
| `min_wall_qty` | `min_wall_qty` | 30.0 |
| `drop_threshold` | `drop_threshold` | 0.9 |
| *(none)* | `depth_levels_` | 5 |

`EvaluateCandle` returns an empty signal, so the candle path never produces
anything from this trigger. `EvaluateTick`:

1. `const auto& current_l2 = context.latest_l2`; if `timestamp == 0` return
   `NONE`.
2. Walk `l2_history` from `i = 0` upward (newest → oldest); stop at the first
   snapshot with `current_l2.timestamp - snap->timestamp >= lookback_ms_`.
3. `check_depth = std::min(depth_levels_, MAX_L2_DEPTH)` = 5.
4. Sum `qty` over `bids[0..5)` and `asks[0..5)` for past and current.
5. Past ask sum `>= min_wall_qty_` **and** current ask sum
   `<= past * (1 - drop_threshold_)` → `BUY`.
6. Mirror for bids → `SELL`.

Tuning constraints that follow directly from the code:

- **Aggregate, not wall detector.** One level pulling out is invisible if the
  other four absorbed it.
- **Ask is checked first.** A simultaneous collapse on both sides reports `BUY`
  and never reaches the bid branch.
- **`min_wall_qty` compares against the top-5 sum** — instrument- and
  venue-dependent, see §5.1.
- **Cancel and fill are indistinguishable.** No cross-check against the tape.
- **Direction only.** No entry/stop/target/volume; the funnel supplies those.
- **No valid-level guard.** Five slots are always summed because `L2Snapshot`
  has no counts and the loop does not test `price > 0`.

### 5.1 What "top 5" means per venue

| venue | slots summed | real levels | effective meaning |
|-------|-------------:|------------:|-------------------|
| Bybit | 5 | **1** | `past_ask_qty` is a single best-level quantity. `min_wall_qty = 30` must be met by **one** level — on BTC that is an enormous wall, so the trigger effectively never fires. |
| Hyperliquid | 5 | up to 5 | genuine top-5 aggregate; a thin book degenerates to the Bybit case. |
| Binance | 5 | 1 | `bookTicker` is top-of-book by construction. |

A parameter set tuned on Hyperliquid does not reproduce on Bybit. They are
different strategies sharing a class name.

---

## 6. Consumers of the book

| consumer | kind | reads | depth | path |
|----------|------|-------|------:|------|
| `SpoofHunterTrigger` | trigger | `bids[i].qty`, `asks[i].qty` | 5 | tick |
| `OrderbookImbalanceFilter` | filter | `best_bid_qty`, `best_ask_qty` | 1 | tick **and** candle |
| `StackedImbalanceTrigger` | trigger | footprint | 0 | candle |
| `DeltaAbsorptionTrigger` | trigger | footprint | 0 | candle |
| `OR_Trigger` | combinator | delegates | — | tick + candle |
| `AND_Trigger` | combinator | delegates | — | **candle only** |

`SpoofHunterTrigger` is the only *trigger* that reads the snapshot, and
`OrderbookImbalanceFilter` is the only one of the ten filters in
`include/filter.hpp` that touches `latest_l2`. The other nine read only
`context.history`, `context.session` or `context.live_bar`. Nothing in the tree
reads `Bar::last_l2` — it is written by `ProcessTrade` and after `ResetBar`, and
dereferenced by nobody.

The ten filters, for completeness: `MinVolume`, `RVOL`, `ATR`, `TimeOfDay`,
`POCTrend`, `MacroTrend`, `Volatility`, `VwapTrend`, `CVDDivergence`,
`OrderbookImbalance`.

### 6.1 `StackedImbalanceTrigger` and `DeltaAbsorptionTrigger`

Neither touches `L2Snapshot`; both read `context.history.back().footprint`,
built by the aggregator from `TradeEvent` prints.

- **`StackedImbalance`** is **direction only** — it returns
  `TradeSignal{SignalDirection::BUY}` with no price, stop or target, and relies
  on the funnel to derive them. It counts unbroken runs of same-side imbalance
  across adjacent footprint levels, breaking the run when the price gap exceeds
  `1.5 × tick`.
- **`DeltaAbsorption`** is **not** direction-only: it sets `entry_price =
  bar.close`, `stop_loss = bar.low - 2×tick` (or `bar.high + 2×tick` for shorts)
  and a 2:1 `take_profit`. It fires when cumulative delta is strongly one-sided
  while the candle closes the other way.

`OR_Trigger` overrides both `EvaluateCandle` and `EvaluateTick`, so it works on
the tick path. `AND_Trigger` overrides **only** `EvaluateCandle` — composed
inside an `AND`, SpoofHunter's tick signal can never be produced, because
`AND_Trigger::EvaluateTick` inherits the empty base implementation.

### 6.2 `OrderbookImbalanceFilter`

Config: `OrderbookImbalance`, `ratio = 3.0`. `include/factory.hpp` flags it:
*"<- a FILTER; not a valid trigger name"*.

The body, verbatim:

```cpp
if (intended_signal.direction == SignalDirection::BUY) {
    if (l2.best_ask_qty <= 0.0) return true;          // fails OPEN
    return (l2.best_bid_qty / l2.best_ask_qty) >= required_ratio_;
} else if (intended_signal.direction == SignalDirection::SELL) {
    if (l2.best_bid_qty <= 0.0) return true;          // fails OPEN
    return (l2.best_ask_qty / l2.best_bid_qty) >= required_ratio_;
}
return false;
```

Both zero guards return **`true`**, so the filter deliberately fails open:

| book state | BUY | SELL |
|------------|-----|------|
| bid 30 / ask 10, ratio 3.0 | `30/10 >= 3` → **allowed** | `10/30 < 3` → blocked |
| bid 20 / ask 10, ratio 3.0 | `20/10 < 3` → blocked | `10/20 < 3` → blocked |
| zeroed gap frame (0 / 0) | ask guard → **allowed** | bid guard → **allowed** |
| one-sided real book (ask 0) | ask guard → **allowed** | `0/bid < 3` → blocked |

Consequences:

- It is a **depth-1 consumer**, so it sees identical data on every venue and
  ignores Hyperliquid's deeper book entirely.
- **It gates SpoofHunter.** `PipelineStrategy::OnTickUpdate` runs every
  registered filter on a tick signal, so a top-5-derived direction can be
  vetoed by an L1 ratio check on the same snapshot.
- It also runs on the candle path, where it gates the footprint triggers.
- **A one-sided book is waived, not blocked.** A real book with bids but no asks
  passes BUY unconditionally, and vice versa. The filter cannot distinguish a
  half-formed book from a healthy one.
- **It and the gap gate disagree about what a gap is.** `WorkerLoop` classifies
  a frame by `best_bid_price`/`best_ask_price`; this filter classifies by
  `best_bid_qty`/`best_ask_qty`. A frame with prices set and quantities zero is
  a gap to the filter but healthy to the worker — and vice versa.
- On a zeroed gap frame containment is entirely `execute_signal`'s `BookGap()`
  refusal (§8). The filter is not a second line of defence; it is a hole.

`test/test_filters.cpp` pins the first two rows: bid 30 / ask 10 allows BUY and
blocks SELL at ratio 3.0; bid 20 / ask 10 blocks BUY.

Full path of one live tick signal:

```
bids[0..4] / asks[0..4]        -> SpoofHunterTrigger       -> direction
best_bid_qty / best_ask_qty    -> OrderbookImbalanceFilter -> veto
best_ask_price / best_bid_price -> execute_signal          -> fill price
```

### 6.3 `BookView` — the better abstraction, currently unused

`include/market_data/book_view.hpp` carries everything `L2Snapshot` lacks:

```cpp
struct BookView {
  const BookLevel* bids;  const BookLevel* asks;
  uint32_t n_bids, n_asks;
  int64_t  ts_exchange_ns, ts_recv_ns, seq;
  uint8_t  flags;

  bool valid() const;  bool empty() const;
  double best_bid() const;  double best_ask() const;
  double depth_qty(uint32_t k, bool bid_side) const; // clamps k to n_*
};
```

`depth_qty` is exactly the primitive a sum-of-top-k trigger needs, because it
clamps instead of over-reading. **Nothing in the tree references `BookView`**;
`MarketContext` still exposes `const L2Snapshot &latest_l2`.

The same header carries the startup gate —
`DataCapabilities`, `TriggerRequirements`, `MeetsRequirements(caps, req)` — whose
own comment says it exists *"so that a depth-starved trigger aborts with a
readable error instead of silently returning SignalDirection::NONE forever."*
Nothing wires a `TriggerRequirements` for `SpoofHunterTrigger`, so that abort
never happens.

---

## 7. Strategy layer

`PipelineStrategy` is the only filter host, and it applies filters on **both**
paths with identical shape:

```cpp
// OnTickUpdate
TradeSignal signal = trigger_->EvaluateTick(context);
if (signal.direction == SignalDirection::NONE) return TradeSignal{};
for (auto *filter : filters_) {
  if (!filter->AllowTrade(context, signal)) return TradeSignal{};
}
return signal;
```

Consequences:

- **Filters veto, they never modify.** The returned signal is the trigger's own,
  so a filter can turn BUY/SELL into NONE but cannot touch entry, stop or
  target. `DeltaAbsorption`'s own SL/TP survive the chain untouched.
- A trigger with no setup still short-circuits before any filter runs, which is
  why a filter cannot be used to *count* trigger evaluations.
- `AlphaEngine::Evaluate(ctx, candle_finished)` is a pure router — it holds no
  filters, no trigger and never reads the book. It only chooses
  `OnCandleClose` vs `OnTickUpdate`.

---

## 8. The live worker, branch by branch

`src/live_engine.cpp`, `WorkerLoop`. Verified behaviour:

### 8.1 `Event::Type::L2`

1. `ProcessL2` to **both** `aggregator` and `htf_aggregator`. Book-first
   ordering: the snapshot that triggers an evaluation is already
   `get_history(0)`, not one tick behind.
2. Gap check: `best_bid_price <= 0.0 || best_ask_price <= 0.0` → latch
   `book_gap_`, `break`. **The prop guard is skipped on this frame.**
3. Otherwise clear `book_gap_` if set, logging recovery.
4. `CheckPropGuard(l2.timestamp, best_bid_price > 0 ? best_bid_price : best_ask_price)`
   — the account limit is observed on book updates as well as on prints.
5. If `live_bar_.close == 0.0`, `break` — **tick alpha cannot fire before the
   first trade initialises the bar.**
6. `AlphaEngine::Evaluate(tick_ctx, false)` → `OnTickUpdate`.
7. On a signal, `exec_price = best_ask_price` (BUY) or `best_bid_price` (SELL),
   falling back to `live_bar_.close`, then to `best_bid_price`.

### 8.2 `Event::Type::TRADE`

1. `CheckPropGuard(trade.timestamp, trade.price)` — **before** anything acts on
   the print, so a breach refuses the trade rather than racing it.
2. `pos_manager->Update` (trailing / break-even / scale-out bookkeeping).
3. `ActiveBroker()->CheckRisk` (SL/TP/trail/scale-out); publish
   `POSITION_CLOSED` when a full close happened.
4. `ProcessTrade` on both aggregators → `candle_finished`.
5. Bar guard (`close == 0.0`) → `break`.
6. `Evaluate(ctx, candle_finished)` → `OnCandleClose` or `OnTickUpdate`.
7. On a signal, `exec_price = trade.price`.

### 8.3 `execute_signal`

```
refuse if BookDegraded() || BookGap()      <- stale book outranks everything
refuse if !prop_guard_.allow_new_entry()   <- account limit outranks strategy
SignalExecutor::Execute(signal, exec_price, ts)
```

`SignalExecutor` is the single shared funnel (stop validation → sizing → risk
firewall → stop re-validation → take-profit derivation) used by live and
backtest alike.

### 8.4 Two construction details worth knowing

- `MarketContext` is built positionally as
  `{history, live_bar_, htf_live_bar_, session, latest_l2, l2_history}` — and
  `live_bar`/`htf_bar` are both `const Bar&`. **Swapping them compiles
  silently** and would feed the higher-timeframe bar to every trigger.
- `Event` is a tag plus a union of `TradeEvent` (32 B, `alignas(32)`) and
  `L2Snapshot` (688 B, `alignas(16)`); the union is 32-aligned, so the payload
  occupies 704 B and `sizeof(Event)` is 736. The 65,536-slot queue therefore
  costs ~48.2 MB at saturation, and trade-only events pay the `L2Snapshot`
  width.

---

## 9. Gap and degradation handling

All three live adapters emit `MakeBookGap()` on reconnect, unexpected close and
error: a `memset`-zeroed `L2Snapshot` with a **live** timestamp.

```cpp
L2Snapshot gap{};
std::memset(&gap, 0, sizeof(L2Snapshot));
gap.timestamp = NowMillis();
```

Traced through SpoofHunter alone that is a `BUY`: the timestamp guard passes,
`past_l2` resolves to a genuine pre-drop snapshot with
`past_ask_qty >= min_wall_qty`, and `current_ask_qty == 0` satisfies the drop
threshold.

Three verified mechanisms contain it:

1. `WorkerLoop` classifies `best_*` **price** <= 0 as a gap, latches `book_gap_`,
   and `break`s before evaluating alpha.
2. `execute_signal` refuses every signal while `BookDegraded() || BookGap()`.
3. Because `ProcessL2` runs *before* the gap check, the zeroed frame is in the
   ring buffer. For the first 500 ms after recovery every real snapshot is
   younger than the lookback, so the walk continues past them and selects the
   **gap** as `past_l2` — `past_ask_qty == 0` is below `min_wall_qty`, so no
   signal is produced. Once 500 ms of fresh data exists the walk stops at a real
   snapshot and the gap is never consulted again.

| latch | set by | cleared by | effect |
|-------|--------|------------|--------|
| `book_gap_` | a gap frame | the next real snapshot | entries refused (recoverable) |
| `book_degraded_` | an event dropped on queue overflow | **never** | entries refused for the rest of the run |

Sharp edges:

- The gap is recognised **only through the legacy `best_*` price fields**, while
  `OrderbookImbalanceFilter` recognises it through `best_*_qty`. The two guards
  disagree about what a gap is (R6).
- The gap check `break`s *before* `CheckPropGuard`, so a disconnect also pauses
  drawdown observation. An open position stays protected by the venue-side stop,
  but a breach during the outage is only noticed once data resumes.
- No equivalent gate exists on the backtest/replay path, where a zeroed snapshot
  with a real timestamp reaches the trigger directly.

---

## 10. Live adapters

All three share the same skeleton: `enableAutomaticReconnection()`, a gap frame
on every `Open` after the first, and on `Close`/`Error` while `running_` is
still set — so an operator-initiated `Stop()` is not reported as a gap.

### 10.1 Bybit — `src/bybit_streamer.cpp`

- Subscribes `publicTrade.<symbol>` and **`orderbook.1.<symbol>`**.
- `orderbook.1` is depth 1. The other 19 slots stay zero from the `memset`;
  `best_*` is filled and copied into `bids[0]`/`asks[0]`.
- Timestamp: `data["ts"]` (matching-engine ms), wall-clock ms fallback.
- `"S"` is the **taker** side: `is_buyer_maker = (S == "Sell")`.
- Ignores the frame `type` (`snapshot` vs `delta`) and drops any frame with an
  empty side. At depth 1 each frame carries both sides, so this works — see R12.
- App-level `{"op":"ping"}` every 20 s from a dedicated thread, slept in 100 ms
  slices so `Stop()` is never blocked for a full interval.

### 10.2 Hyperliquid — `src/hyperliquid_streamer.cpp`

- Subscribes `l2Book` **and** `trades` as two separate frames (`subscription`
  accepts a single object). The trades feed is not optional: the bar aggregator
  only advances on `TradeEvent`s, so an `l2Book`-only session closes no candle
  and evaluates no trigger — indistinguishable from a quiet market.
- `l2Book` is a **full snapshot per push**; no delta maintenance.
- Up to 20 levels per side, best-first (bids descending, asks ascending), copied
  verbatim and capped at `MAX_L2_DEPTH`; tail stays zeroed.
- Drops a frame only if a side is entirely empty; a 2-level book is accepted and
  leaves 18 zero slots.
- Timestamp: `data["time"]` (ms), wall-clock ms fallback.
- No `nSigFigs`/`mantissa`, so level aggregation is the venue default and the
  returned depth is not controlled by us.
- `"side"` is the **aggressor**: `"B"` lifted the ask, `"A"` hit the bid; mapped
  as `is_buyer_maker = (side == "A")`.
- Protocol-level ping every 30 s (`ixwebsocket`).
- Minor: a level missing `px`/`sz` is skipped with `continue`, leaving a zeroed
  hole at that index while later levels keep theirs.

### 10.3 Binance — `src/binance_streamer.cpp`

- Default URL
  `wss://stream.binance.com:9443/stream?streams=btcusdt@aggTrade/btcusdt@bookTicker`.
- `bookTicker` is **top of book**: one price/qty per side, no depth.
- `HandleDepth` fills `best_bid_price`/`best_bid_qty`/`best_ask_price`/
  `best_ask_qty` and mirrors them into `bids[0]`/`asks[0]`, exactly as Bybit
  does. Slots 1–19 stay zero from the `memset`, so `depth_levels_ = 5` sums one
  real level and four zeros — the Bybit case again.
- Timestamp: `data["E"]` (event time, ms) when present and integral, otherwise
  wall-clock ms. **Spot `bookTicker` carries no `E`**, so a spot session's book
  timestamps come from the local clock while its `aggTrade` timestamps come from
  venue `"T"`. Futures takes both from the venue. This is the one place in the
  tree where the book and trade clock domains can diverge (R10).
- The symbol is baked into the default URL and `factory.cpp` constructs
  `BinanceDataStreamer()` with no arguments, so **every** binance config streams
  `BTCUSDT` regardless of `cfg.symbol`.
- `HandleDepth` never validates the `"s"` field, so a payload for another symbol
  would be accepted. Moot with a single-symbol URL, latent if one is ever passed.
- `OnMessage` requires both `"stream"` and `"data"` — the combined-stream
  envelope — so a raw per-stream payload is dropped.
- `HandleAggTrade` reads `"T"` (trade time, ms) and uses `"m"` directly as
  `is_buyer_maker`, matching `Aggregator::UpdateBarData`.
- No application-level ping (Binance answers protocol pings itself); no
  heartbeat thread as on Bybit.

---

## 11. CSV ingestion

### 11.1 Trades — `CSVLoader::ProcessBinanceCSV` (the only row processor)

Correct on all four counts:

- `trade.timestamp = NormalizeEpochToMillis(std::stoll(col[cols.ts]))` — the
  ns/µs/s/ms normaliser **is** applied.
- Aggressor: a `maker` column is read as `is_buyer_maker`; a `side` column is
  inverted via `ParseSideToken(side) < 0`, because Bybit and Hyperliquid name
  the **taker**. Matches `Aggregator::UpdateBarData`, where `is_buyer_maker`
  increments `bid_volume` and decrements `cumulative_delta`.
- BOM stripped, trailing `\r` removed (the maker string comparison would
  otherwise fail on CRLF files), short rows dropped, per-row parse failures
  swallowed so one bad line cannot abort a replay.
- Header detection: labelled headers from Binance, Bybit and Hyperliquid are
  matched on lowercase-alphanumeric labels; headerless Binance Vision
  `trades`/`aggTrades`, headerless Bybit public trades (both field orders) and
  headerless Hyperliquid fills are recognised by shape rules.

Limits that matter: only the first 32 columns are read; `quote_*` columns are
excluded from qty deliberately (notional, not quantity); a header matching only
*some* fields still sets `has_header = true`, so the caller rejects the file
instead of replaying the label row as data.

### 11.2 Books — no load path

`SniffL2Columns` exists and maps book columns:

- labelled: every bid/ask price+qty column collected **in file order**, so a
  wide-format multi-level file (`bid_price_1`, `bid_price_2`, …) yields one
  vector entry per level;
- headerless: canonical Binance Vision bookTicker
  `update_id,bid_px,bid_qty,ask_px,ask_qty,transact_time,event_time` mapped to
  indices 1–4;
- a 7-column row whose last field is a boolean word is a trades file, rejected
  before the book heuristics run.

But:

1. **Nothing consumes it.** `CSVLoader` declares only `ProcessBinanceCSV`,
   `SniffTradeColumns` and `SniffL2Columns`. There is no `ProcessL2CSV`, and no
   file in the tree calls `SniffL2Columns`.
2. **`CsvColumns` has no timestamp index for L2.** The bookTicker row's
   `transact_time`/`event_time` at indices 5–6 are never captured, so a loader
   built on this sniffer could not set `L2Snapshot::timestamp` — and
   `timestamp == 0` makes SpoofHunter return `NONE` forever.
3. Header order is taken as level order. A deepest-first file would be reversed
   relative to `bids[0] = best`.

**Trade CSVs parse correctly; book CSVs are sniffed but not loaded.** A backtest
that believes it is using a book CSV silently runs with an empty book.

### 11.3 Timestamp normaliser

| input | interpretation |
|-------|----------------|
| `>= 1e17` | nanoseconds → `/ 1e6` |
| `>= 1e14` | microseconds → `/ 1e3` |
| `< 1e11` | seconds → `* 1000` |
| otherwise | already milliseconds |

Returns `ts` unchanged for `ts <= 0`. Header rationale: a microsecond export used
to pass a `> 1.5e12` plausibility check and was read as milliseconds, dating
rows to the year 55000.

All three live adapters already emit milliseconds, so this is a no-op on the
live path. It matters only at file ingest, where it is applied to trades but
never to books.

---

## 12. On-disk format: `.mkn`

`include/market_data/format.hpp` describes bytes on disk only. The normative
spec is `docs/market_data_format.md`. The extension is **`.mkn`**;
`kBlockMagic` spells ASCII `'M','K','N','B'` little-endian, which is where the
informal "mknb" name came from. `git ls-files` finds no `mkn` path — the format
is currently a specification with no reader or writer by that name in the tree.

| Constant | Value |
|----------|-------|
| `kFileMagic` | `"MOKUREN\0"` |
| `kBlockMagic` | `0x424E4B4D` (`MKNB` LE) |
| `kFormatVersion` / `kMinReaderVersion` | 1 / 1 |
| `kMaxBookDepth` | **20** |
| `kFileHeaderSize` / `kBlockHeaderSize` / `kIndexEntrySize` | 128 / 56 / 32 |
| `sizeof(TradeRec)` / `BookSnapshotRec` / `BookChange` / `BookDeltaRec` / `BookLevel` | 48 / 40 / 24 / 32 / 16 |

**Every `ts_*` in the file is nanoseconds since epoch.** The engine works in
milliseconds, so a reader performs one integer shift at the boundary.

- `RecordKind`: `Trade`, `BookSnapshot`, `BookDelta`, plus reserved `Mark` and
  `SessionMarker`.
- `BlockCodec`: `None`, `Zstd`.
- `Aggressor`: `Unknown`/`Buy`/`Sell` — never a venue `is_buyer_maker` boolean,
  because that flips meaning per venue and inverts footprint, delta, POC and
  every imbalance threshold.
- `Granularity`: `Tob`, `Mbp`, `Mbo`. **`Tob` means depth 1 per side — the file
  layer can express exactly the Bybit/Binance reality that `L2Snapshot`
  cannot.**
- `BookEncoding`: `Snapshot`, `SnapshotPlusDelta`.
- `BookSnapshotRec` carries `n_bids`, `n_asks`, `seq`, `flags`, `ts_recv_ns`.
  Block stride reserves the block-wide maximum; unused level slots are
  zero-filled and ignored. `n_*` must be `<= kMaxBookDepth`.
- `BookDeltaRec` carries `n_changes` and `BookChange[]` (`side` bid/ask,
  `action` set/remove; qty ignored on remove).
- `snapshot_flags`: `kResync`, `kTruncated`, `kStale`;
  `trade_flags`: `kLiquidation`, `kBlockTrade`.

**The asymmetry that matters:** the file format has level counts, sequence
numbers, flags and a TOB granularity; `L2Snapshot` has none of them. That
information is discarded at the adapter boundary and is the root of most risks
in §14.

Deriving an `L2Snapshot` from a record: copy the `n_*` valid levels best-first
and **zero-fill the rest**. Zero-fill is the only safe choice — stale leftovers
create phantom walls.

---

## 13. Quick reference

| Name | Value | Where |
|------|------:|-------|
| `kMaxBookDepth` / `MAX_L2_DEPTH` | 20 | format.hpp / trade_event.hpp |
| `sizeof(L2Level)` | 16 | trade_event.hpp (asserted) |
| `sizeof(L2Snapshot)` | 688 | trade_event.hpp (asserted) |
| `alignof(L2Snapshot)` | 16 | trade_event.hpp (asserted) |
| `sizeof(TradeEvent)` | 32 | trade_event.hpp (`alignas(32)`) |
| `L2_RING_BUFFER_SIZE` | 1024 | aggregator.hpp |
| ring buffer bytes | 704,512 each (×2 live) | derived |
| `kMaxQueueDepth` | 65,536 | live_engine.hpp |
| `sizeof(Event)` | 736 | derived |
| queue worst case | ≈ 48.2 MB (46 MiB) | derived |
| max snapshots/s for 500 ms lookback | 2048 | derived |
| `MAX_GRID_LEVELS` | 5000 (×24 B ≈ 117 KiB) | aggregator.hpp |
| SpoofHunter `depth_levels_` | 5 | trigger.hpp |
| SpoofHunter `lookback_ms_` / `min_wall_qty_` / `drop_threshold_` | 500 / 30.0 / 0.9 | trigger.hpp |
| OrderbookImbalance `ratio` | 3.0 | factory.hpp |
| Bybit orderbook topic | `orderbook.1` (depth 1) | bybit_streamer.cpp |
| Binance stream | `bookTicker` (depth 1) | binance_streamer.cpp |
| Bybit heartbeat | 20 s, 100 ms slices | bybit_streamer.cpp |
| Hyperliquid ping interval | 30 s | hyperliquid_streamer.cpp |
| file `ts_*` unit | ns | format.hpp |
| engine unit | ms | aggregator.cpp, main.cpp |

---

## 14. Risks, ordered by severity

**R1 — No valid-level guard; "top 5" is venue-dependent.**
`L2Snapshot` has no counts and the loop always sums 5 slots. On Bybit and
Binance four are structurally zero on every snapshot; after a gap all five are.
`min_wall_qty = 30` therefore means "one best-level quantity ≥ 30" on those
venues and "sum of five levels ≥ 30" on Hyperliquid. The trigger is not
comparable across venues and can look inert purely because of the feed.
*Fix:* carry `n_bids`/`n_asks` (the file format and `BookView` already do), or
gate at startup with `MeetsRequirements`.

**R2 — SpoofHunter depth is not configurable.**
`depth_levels_ = 5`, private, no constructor parameter, no setter, no config key.
Using 20 requires editing the class. `MakeTrigger` passes exactly three
arguments; a `depth_levels` key in JSON is read by nobody and silently ignored.

**R3 — `best_*` is load-bearing, not legacy.**
`WorkerLoop` detects a gap via `best_bid_price <= 0.0 || best_ask_price <= 0.0`.
An adapter that fills the arrays but not `best_*` makes *every* frame look like a
gap: `book_gap_` never clears and the run takes no entries, silently. The reverse
passes the gate and feeds SpoofHunter an empty book. All three current adapters
fill both consistently; nothing enforces it for a fourth.

**R4 — The gap sentinel carries no testable marker.**
`MakeBookGap()` is all-zero arrays with a live timestamp. Containment depends on
three callers remembering to check. A `flags` or count field reading 0 would make
it self-describing. No gate exists on the backtest/replay path at all.

**R5 — Book CSVs cannot be loaded.**
No `ProcessL2CSV`, no caller of `SniffL2Columns`, no L2 timestamp column in
`CsvColumns`. A backtest that expects a book CSV runs with an empty book and
returns `NONE` forever.

**R6 — The depth-1 filter gates the depth-5 trigger, and fails open.**
`OrderbookImbalance` reads only best-level quantities and vetoes SpoofHunter on
the tick path: direction from L2 depth 5, veto from L1 depth 1, fill price from
`best_*`. Its zero guards return `true` — `if (best_ask_qty <= 0) return true`
for BUY, mirrored for SELL — so a zeroed gap frame passes **both** directions,
and a one-sided real book passes the side whose book is missing. Containment is
entirely `execute_signal`'s `BookGap()`, which tests `best_bid_price` /
`best_ask_price` while the filter tests quantities: the two guards disagree
about what a gap is.

**R7 — Lookback capacity cliff.**
Above ~2048 snapshots/s the 500 ms window is unreachable and the trigger is
permanently silent. Hyperliquid pushes on every book change. Needs a per-venue
rate measurement, not an assumption.

**R8 — Lookback quantisation.**
Effective lookback is `ceil(500 / period) * period`, not 500 ms. At 200 ms this
is 600 ms — a 20 % error in the only tuning parameter with a physical meaning.

**R9 — `ProcessL2` trusts the feed completely.**
No dedupe, no monotonicity, no sequence, no flag handling. A replayed old
snapshot poisons every lookback window containing it. Bybit sends `u`/`seq`;
nothing stores them.

**R10 — Book/trade clock domains can diverge on Binance spot.**
`HandleDepth` uses `"E"` when present and wall-clock otherwise; spot
`bookTicker` has no `"E"`, while `HandleAggTrade` uses venue `"T"`. On spot the
book is stamped locally and the trades are stamped by Binance. The only coupling
between the two domains is that book timestamps must be self-consistent for
`lookback_ms_`, so this does not break a join today — because there is no join —
but it means a wall pulled at venue time T is compared against locally stamped
book updates.

**R11 — Cancel/fill ambiguity.**
A wall pulled and a wall consumed by aggressive orders are identical to the
trigger; there is no cross-check against the tape.

**R12 — Bybit delivers one level and cannot simply be widened.**
`orderbook.50` switches the venue to snapshot-then-**delta** frames with size
`"0"` meaning removal, which the current handler drops as "empty side". Real L2
on Bybit needs `type` handling and a level-maintenance loop, not a topic-string
edit.

**R13 — No validation in any live adapter.**
No ordering, crossed-book, duplicate-timestamp or monotonic-seq checks before
`EmitL2`. They forward the venue's array as-is. Binance additionally never
checks the `"s"` symbol field.

**R14 — `AND_Trigger` never evaluates tick signals.**
It overrides only `EvaluateCandle`, so a composed pipeline containing
SpoofHunter silently produces nothing on the tick path.

**R15 — `MarketContext` is built positionally, and two members share a type.**
`live_bar` and `htf_bar` are both `const Bar&`; swapping them in the six-argument
aggregate initialisation compiles cleanly and feeds the wrong bar to every
trigger.

**R16 — Queue memory is ~48.2 MB at saturation.**
Trade-only events pay the `L2Snapshot` width because the payloads are a union.

**R17 — Streamers are configured asymmetrically.**
Bybit takes symbol + host; Binance takes neither (hardcoded `btcusdt`);
Hyperliquid takes a bare coin from `cfg.symbol`. The same `symbol` key means
different things per venue, and a binance config cannot select its instrument.

**R18 — The prop guard is skipped on gap frames.**
The L2 branch `break`s before `CheckPropGuard`, so a disconnect pauses drawdown
observation until data resumes.

**R19 — SpoofHunter has no test coverage.**
`test/test_filters.cpp` covers filters only. The two cases a `L2RingBuffer` +
`SpoofHunterTrigger` unit test would pin — the depth-1 book of Bybit/Binance and
the gap-frame-as-`past_l2` interaction — are exactly the two that matter.

**R20 — `BookView` is not wired in.**
The counted, clamping abstraction exists but nothing uses it. Migrating
`MarketContext` to it would resolve R1 and R9 structurally and would let the
TOB/MBP distinction survive the adapter boundary.

---

## 15. Checklist for a new streamer

1. Emit `L2Snapshot` with `bids` best-first descending and `asks` best-first
   ascending — and validate it, because nothing downstream will.
2. Fill all 20 slots; zero-fill the tail; never leave stale values.
3. Fill **both** `best_bid_*`/`best_ask_*` and `bids[0]`/`asks[0]`. The gap gate
   reads `best_*_price`; the imbalance filter reads `best_*_qty`; SpoofHunter
   reads the arrays. All three must agree.
4. Emit `timestamp` in **milliseconds**, monotonic, from the venue clock — not
   the local clock, or book and trade domains split as on Binance spot.
5. Never emit an older snapshot than the last; `ProcessL2` will not filter it.
6. Check the rate against `R <= 2048`/s for a 500 ms lookback.
7. If the venue publishes fewer than 20 levels, record it in metadata
   (`Granularity::Tob`, `n_bids`/`n_asks`) rather than truncating implicitly —
   and remember Bybit and Binance are the depth-1 case today.
8. Do not repack `L2Snapshot`; the static asserts and the cache filename bump are
   the guard rails.
9. Map aggressor → `is_buyer_maker` by inverting the **taker** side, as all three
   adapters and `CSVLoader` do.
10. Emit a gap sentinel on reconnect/close/error — and make it distinguishable
    from a real collapse. An all-zero snapshot with a valid timestamp is read as
    a 100 % collapse.
11. Subscribe to a trade feed as well. The bar aggregator only advances on
    `TradeEvent`s; a book-only subscription closes no candle.

---

## 16. Remaining unknowns

Not a source file — an artefact:

- **The `.mkn` reader/writer.** `git ls-files` finds no path matching `mkn`, so
  §12 describes a format with no implementation by that name in the tree. If one
  exists under another name it needs checking for two things: clamping
  `n_bids`/`n_asks` to `kMaxBookDepth`, and shifting the nanosecond timestamps to
  milliseconds before they reach `ProcessL2`.
