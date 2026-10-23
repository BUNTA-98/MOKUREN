# Stream Validation

How market data is checked before and while the engine trades, what each
failure means, and how to change it without breaking the guarantee.

- [1. Why this exists](#1-why-this-exists)
- [2. The guarantee, precisely](#2-the-guarantee-precisely)
- [3. Where the code lives](#3-where-the-code-lives)
- [4. Data flow](#4-data-flow)
- [5. The verdict model](#5-the-verdict-model)
- [6. Validation rules](#6-validation-rules)
- [7. The book-gap sentinel](#7-the-book-gap-sentinel)
- [8. Venue profiles](#8-venue-profiles)
- [9. Probe criteria and the gate](#9-probe-criteria-and-the-gate)
- [10. The report](#10-the-report)
- [11. Running-phase validation](#11-running-phase-validation)
- [12. Configuration reference](#12-configuration-reference)
- [13. Failure policy and exit behaviour](#13-failure-policy-and-exit-behaviour)
- [14. Operability](#14-operability)
- [15. Troubleshooting](#15-troubleshooting)
- [16. Adding a venue](#16-adding-a-venue)
- [17. Testing](#17-testing)
- [18. Known limitations and non-goals](#18-known-limitations-and-non-goals)

---

## 1. Why this exists

There used to be a separate `venue_doctor` executable that connected to a venue
and reported whether bytes arrived. That answers "can I reach the venue". It
does not answer "is this payload legal input for the engine". Those diverge
exactly when it matters: a renamed field still produces bytes, still passes a
connectivity check, and then poisons the aggregator with a `NaN` price or a
book whose side is mis-sorted.

This layer replaces that tool. Its checks are compiled into the same binary
that trades, so a preflight verdict and a live verdict cannot disagree about
what "valid" means.

Two design decisions follow from that:

1. **One implementation, three call sites.** The same `ValidateTrade` /
   `ValidateL2` functions run during startup checks, during the live run, and
   (once wired) during replay decoding.
2. **The probe is the connection.** The startup checks do not open a throwaway
   socket. A `FeedRouter` becomes the streamer's only callback target *before*
   the streamer starts and holds every frame back until the gate passes. The
   connection that passes the checks is the connection that trades, so no load
   balancer or rate limiter gets a second chance to behave differently.

What this layer deliberately does **not** do: it does not judge strategy
semantics, it does not reconcile snapshot+delta books *itself* — that is
per-venue work and lives in the streamer, see
[§6.4](#64-snapshotdelta-book-maintenance-producer-side) — it does not verify
`tick_size` against venue metadata, and it does not decide whether a *signal*
is worth taking.

---

## 2. The guarantee, precisely

**Before the engine is armed:**

- Every frame the streamer emits passes through the router.
- No frame reaches the engine until the gate reports `Ready`.
- `Ready` requires **every channel the venue profile marks required** to meet
  its own minimum, measured on *clean* frames, for at least `min_duration_ms`,
  with no fatal frame and within the reconnect budget.
- A required channel that stays silent fails at the deadline. It does not pass
  by silence.
- A required channel that produces only rejected frames also fails at the
  deadline, because rejected frames do not count toward the minimum.

**After the engine is armed:**

- Every frame is still validated before it is forwarded.
- A **fatal** frame stops the run through the existing stream-error path
  (`OnStreamError`), exactly like a transport failure. It is reported once, not
  once per bad frame.
- A **recoverable** frame is dropped and counted. The engine keeps its last
  good book.

**At the handoff:**

- The router emits a book-gap sentinel to the engine immediately before it
  starts forwarding. The engine therefore never trades against a book it did
  not see built; it waits for a fresh snapshot.

Frames that arrive between the gate reaching `Ready` and `BeginRunning()` are
discarded (not buffered). That window is at most one poll interval — 20 ms.

---

## 3. Where the code lives

| File | Role |
|---|---|
| `include/data_integrity.hpp` / `src/data_integrity.cpp` | Pure checks (`ValidateTrade`, `ValidateL2`), verdict types, `Code`, the single `MakeBookGap` / `IsBookGap` sentinel definition |
| `include/venue_profile.hpp` / `src/venue_profile.cpp` | Per-venue declared facts (required channels, sequencing, book depth) |
| `include/startup_gate.hpp` / `src/startup_gate.cpp` | Probe criteria, pass/fail policy, per-channel accounting |
| `include/probe_report.hpp` / `src/probe_report.cpp` | Report structs and terminal formatting |
| `include/feed_router.hpp` / `src/feed_router.cpp` | Holds frames during probing, validates while running, flips state |
| `include/live_engine.hpp` / `src/live_engine.cpp` | `Start(criteria)`, poll loop, `PreflightReport()`, abort path |
| `src/main.cpp` | Prints the report, refuses to trade on failure |
| `src/bybit_streamer.cpp` | Bybit's snapshot+delta driver; maps frames onto `OrderBook`, detects gaps, emits complete `L2Snapshot`s (see [§6.4](#64-snapshotdelta-book-maintenance-producer-side)) |
| `include/order_book.hpp` / `src/order_book.cpp` | Venue-agnostic local book: snapshot replace, delta upsert/erase, update-id continuity, truncation to `MAX_L2_DEPTH` |
| `include/resync_watchdog.hpp` / `src/resync_watchdog.cpp` | Backoff that decides when an unusable book justifies reconnecting the socket |
| `include/config_parser.hpp` / `src/config_parser.cpp` | `startup_probe` config block and its cross-checks |
| `test/test_data_integrity.cpp` | Unit tests: validators, gate policy, profiles, report |
| `test/test_feed_router.cpp` | End-to-end: real streamer interface, router, sink |
| `test/test_order_book.cpp` | Unit tests: snapshot/delta application, ordering, gap detection, truncation |
| `test/test_resync_watchdog.cpp` | Unit tests: backoff ladder, deadlines, reset on snapshot |
| `test/test_bybit_live.cpp` | Opt-in live diagnostic (`[.live]`): the real Bybit feed through the real gate |

Namespace: `mokuren::integrity`.

---

## 4. Data flow

### Startup (probing)

```
venue socket
    │  IMarketStreamer::EmitTrade / EmitL2 / EmitFinished / EmitError
    ▼
FeedRouter                 (installed before streamer_->Start())
    ├─ gap sentinel?  ──────────────► gate.ObserveBookGap()  (reconnect counter)
    ├─ ValidateTrade / ValidateL2 ──► gate.ObserveTrade / ObserveL2
    └─ nothing forwarded ───────────► engine sees zero frames
                                        │
main thread: Start(criteria)            │
    poll router.Poll(now) every 20 ms ──┘
        Probing  → sleep, log progress every 5 s
        Failed   → print report, stop streamer, join worker, return false
        Ready    → BeginRunning()
                     ├─ emit book-gap into the engine
                     └─ flip to running
```

### Running

```
venue socket
    ▼
FeedRouter
    ├─ gap sentinel            → forward (engine latches its own gap state)
    ├─ recoverable verdict     → drop, rejected_frames++
    ├─ fatal verdict           → Escalate once → OnStreamError → shutdown
    └─ clean verdict           → orchestrator queue → worker
                                 │
                                 └─ WorkerLoop: aggregators, triggers, risk, broker
```

### Threads and locking

| Thread | Touches |
|---|---|
| Streamer (library) thread | Router callbacks. Run-phase cursors (`run_trade_ts_`, `run_l2_ts_`) are only touched here, so they need no lock. |
| Main thread | `Start(criteria)` poll loop, `Stop()`, `PreflightReport()` |
| Worker thread | Drains the orchestrator queue, feeds aggregators and the strategy |

The gate has its own mutex and is safe to call from the streamer thread and
poll from the main thread. `running_`, `escalated_` and `dropped_running_` are
atomics; `running_` is published with release/acquire so a frame is either
fully probed or fully running, never both.

---

## 5. The verdict model

Every check returns a `Verdict`:

```cpp
struct Verdict {
  Severity severity;   // Ok < Recoverable < Fatal
  Code code;           // machine-readable reason
  bool ok() const;
  bool fatal() const;
};
```

`Severity` is ordered, and `ValidateL2` keeps the **worst** verdict across all
its checks. A book with a zero-quantity level *and* a `NaN` price reports the
`NaN`, not whichever check happened to run last.

| Severity | Meaning | During probe | During run |
|---|---|---|---|
| `Ok` | Frame is legal engine input | counted as `delivered` | forwarded |
| `Recoverable` | Legal-shaped but not trustworthy right now (crossed book, duplicate timestamp, zero qty) | counted as `recoverable`, not delivered | dropped, `rejected_frames++` |
| `Fatal` | Cannot be legal input for any engine state (non-finite, mis-sorted, too shallow) | gate fails immediately | escalates to `OnStreamError`, run stops |

**`Recoverable` is not a warning.** A frame with a recoverable verdict is never
forwarded, because forwarding it would regress the book or double-count a
print. It also does not count toward the channel minimum, so a feed that emits
only crossed books fails the deadline just like a silent one.

---

## 6. Validation rules

### 6.1 Trade prints — `ValidateTrade(const TradeEvent&, int64_t& last_ts)`

Checked in order, first failure wins:

| # | Condition | Severity | Code |
|---|---|---|---|
| 1 | `!isfinite(price)` | Fatal | `TradePriceNotFinite` |
| 2 | `!isfinite(quantity)` | Fatal | `TradeQuantityNotFinite` |
| 3 | `price <= 0` | Fatal | `TradePriceNotPositive` |
| 4 | `quantity < 0` | Fatal | `TradeQuantityNegative` |
| 5 | `quantity == 0` | Recoverable | `TradeQuantityZero` |
| 6 | `timestamp <= 0` | Fatal | `TradeTimestampNotPositive` |
| 7 | `last_ts > 0 && timestamp < last_ts` | Recoverable | `TradeTimestampBackwards` |

`last_ts` advances **only** when the verdict is `Ok`. A rejected print must not
drag the cursor backwards with it, or one stale frame would make every
subsequent in-order print look backwards too.

A single backwards timestamp is recoverable because reordering happens. It is
not tolerated indefinitely: the cursor does not move, so persistent backwards
traffic keeps failing and never counts toward the minimum.

### 6.2 Book snapshots — `ValidateL2(const L2Snapshot&, const BookLimits&, int64_t& last_ts)`

Book layout comes from `include/trade_event.hpp`: `L2Snapshot` has fixed
`bids[MAX_L2_DEPTH]` / `asks[MAX_L2_DEPTH]` arrays and **no per-side count**.
A side ends at the first non-positive price; untouched slots stay zeroed. The
validator uses exactly that convention as its terminator.

| # | Condition | Severity | Code |
|---|---|---|---|
| 1 | `timestamp <= 0` | Fatal | `BookTimestampNotPositive` |
| 2 | `timestamp < last_ts` | Fatal if `require_monotonic_timestamps`, else Recoverable | `BookTimestampBackwards` |
| 3 | level price non-finite | Fatal | `BookLevelPriceNotFinite` |
| 4 | level qty non-finite | Fatal | `BookLevelQuantityNotFinite` |
| 5 | level qty `< 0` | Fatal | `BookQuantityNegative` |
| 6 | level qty `== 0` | Recoverable | `BookQuantityZero` |
| 7 | bid side not strictly descending (equal counts as violation) | Fatal | `BookBidsNotDescending` |
| 8 | ask side not strictly ascending | Fatal | `BookAsksNotAscending` |
| 9 | both sides empty | Fatal | `BookEmpty` |
| 10 | exactly one side empty | Fatal | `BookSideEmpty` |
| 11 | `best_bid > best_ask` | Recoverable | `BookCrossed` |
| 12 | `best_bid == best_ask` | Recoverable | `BookLocked` |
| 13 | either side has fewer than `limits.min_depth` levels | Fatal | `BookTooShallow` |

Notes on the ordering:

- Rules 3–8 are checked per level while scanning, so a mis-sorted side is
  caught even if the frame is otherwise well formed. Depth triggers
  (`StackedImbalance`, `SpoofHunter`) index these arrays **by position**, so a
  mis-sorted side would silently compute garbage rather than fail.
- Rule 13 runs *after* the structural rules, so an empty or one-sided book
  reports `BookEmpty` / `BookSideEmpty` rather than merely `BookTooShallow`.
  Both are fatal; the distinction only matters for diagnosis.
- `best_bid` / `best_ask` for rules 11–12 are **recomputed from the arrays**,
  not read from the legacy `best_*` fields. A snapshot whose legacy fields
  disagree with its arrays is not rejected by this rule, but see
  [§7](#7-the-book-gap-sentinel) — `IsBookGap` reads the legacy fields.
- `last_ts` advances only when the final verdict is `Ok`.

### 6.3 `BookLimits`

| Field | Default | Meaning |
|---|---|---|
| `require_monotonic_timestamps` | `false` | Treat a backwards book timestamp as fatal |
| `min_depth` | `0` | Levels per side required; `0` disables the check |

`min_depth` is the mechanism that stops a depth-1 feed from passing. One
populated level per side is a structurally perfect `L2Snapshot`, so without
this rule a top-of-book venue passes every other check while the depth triggers
read zeroed level-2 slots — a confidently wrong answer rather than a missing
one. See [§8](#8-venue-profiles).

### 6.4 Snapshot+delta book maintenance (producer side)

Not every venue sends a complete book on every frame. Bybit's `orderbook.50`
sends one `"type":"snapshot"` frame and then a stream of `"type":"delta"`
frames, each carrying only the levels whose size changed. Forwarding a delta as
though it were a snapshot hands the engine a book that lost every level the
delta did not mention: structurally valid, clears the depth check, and silently
wrong. `ValidateL2` cannot detect this, because nothing in an `L2Snapshot` says
whether it was complete.

So reconciliation happens in the streamer, **before** validation:

| Frame | What happens |
|---|---|
| no `"type"` field | dropped — there is no safe guess |
| `"type":"snapshot"` | local book cleared and replaced |
| `"type":"delta"`, local book empty | dropped — wait for the replay |
| `"type":"delta"`, `u` skips a value | book reset, gap sentinel emitted, book topic re-subscribed |
| level size `0` | level erased |
| either side empty after applying | nothing emitted: a resync in progress is not a market state |
| otherwise | top `MAX_L2_DEPTH` levels per side, `best_*` taken from the arrays |
| no snapshot after the grace period | socket reconnected, because a new connection always replays a snapshot while a duplicate subscription is not guaranteed to |

The maps are owned by the streamer and reset on every socket open, close and
error, so a reconnect always starts from a fresh snapshot rather than from a
half-remembered book.

Two consequences worth knowing:

- **An update-id gap is a gap, not an error.** It emits the same sentinel as a
  reconnect and re-subscribes. During the probe it counts against
  `max_reconnects`; while running it pauses entries until the new snapshot
  lands, without ending the run.
- **A one-sided book is suppressed, not emitted.** Emitting it would be
  rejected as `BookSideEmpty`, which is fatal, and would end a live run over a
  transient resync. The cost is that L2 can go quiet for a moment with no
  validator error — see [§15](#15-troubleshooting).
- **The apply rules are shared, the parsing is not.** `OrderBook`
  (`include/order_book.hpp`) owns snapshot replace, delta upsert/erase,
  update-id continuity and truncation, and is what `test/test_order_book.cpp`
  exercises off-line — a wrong book produces frames that pass every validator
  rule, so it has to be tested directly. A new delta venue reuses `OrderBook`
  and supplies only the JSON mapping.
- **An unusable book escalates to a reconnect.** Re-subscribing is documented to
  replay a snapshot but a duplicate subscription cannot be relied on; a fresh
  socket always sends one, because that is the handshake the first snapshot
  already depends on. `ResyncWatchdog` waits ~3 s, then forces a reconnect,
  backing off to 60 s so a venue outage does not become a reconnect storm. A
  landed snapshot resets the ladder.

---

## 7. The book-gap sentinel

A streamer emits a **gap** when the local book stops being a faithful picture
of the venue: on reconnect, unexpected close, and transport error. It is *not*
emitted on the initial handshake (both streamers track `first_open`).

The sentinel is defined once, in `data_integrity`:

```cpp
L2Snapshot MakeBookGap(int64_t timestamp_ms);   // every field zeroed, ts set
bool       IsBookGap(const L2Snapshot& s);      // recognises exactly that shape
```

**Producers must not define their own.** Bybit and Hyperliquid each used to
carry a private copy; the consumer recognises the sentinel by *shape*, so two
definitions are two things that must agree forever. Write:

```cpp
L2Snapshot MakeBookGap() { return mokuren::integrity::MakeBookGap(NowMillis()); }
```

### Behaviour

| Where | Effect |
|---|---|
| During probing | Counted as a reconnect by the gate. Does not fail the probe by itself; it is compared against `max_reconnects`. |
| During running | Forwarded to the engine as-is. The orchestrator latches its own gap and suppresses new entries until a real snapshot arrives. |

The gate only counts a gap **after** at least one frame has been delivered
(`trades.delivered + l2.delivered > 0`). This prevents the initial handshake
from being mistaken for a drop if a streamer ever emits it.

### Caveat

`IsBookGap` requires the legacy `best_*` fields **and** every depth slot to be
zero. A frame with zeroed arrays but non-zero legacy fields is not treated as a
gap; it flows to `ValidateL2` and is rejected as `BookEmpty`. A real venue
frame with an all-zero book is not reachable through the validator (price must
be positive), so this only matters if a parser is changed to emit partial
zeros.

---

## 8. Venue profiles

`VenueProfile` is the only place a venue's quirks may influence a verdict. The
checks themselves are shared: a new venue cannot pass by growing a weaker
validator.

```cpp
struct VenueProfile {
  std::string venue = "unknown";
  bool trades_required = true;
  bool l2_required   = true;
  bool has_sequence_numbers = false;
  size_t l2_depth = 0;   // levels per side the feed actually carries
};
```

| Venue | `trades_required` | `l2_required` | `has_sequence_numbers` | `l2_depth` |
|---|---|---|---|---|
| `bybit` | true | true | true | **20** (`orderbook.50`, truncated at `MAX_L2_DEPTH`) |
| `hyperliquid` | true | true | false | **20** (`l2Book`) |
| anything else | true | true | false | 0 (undeclared) |

Two distinct meanings to keep straight:

- **`trades_required` / `l2_required`** — which channels the gate must see.
  Both default to `true`, because a half-blind engine is worse than no engine:
  trades alone cannot drive the depth triggers, and `l2` alone closes no
  candle. An optional channel (`required = false`) never fails the gate and
  never prints `needed N`.
- **`l2_depth`** — a *fact* about the feed, not a requirement. `0` means
  "undeclared": the frame-level depth check still runs against whatever
  arrives, but the gate cannot reject the venue before it sees a frame.

  Declare it **post-truncation**: what the engine can actually receive, not what
  the socket carries. Bybit subscribes to 50 levels and the profile says
  `min(50, MAX_L2_DEPTH)`; claiming 50 would let a `min_l2_depth` of, say, 30
  pass the capability pre-check and then reject every frame as shallow — the
  exact failure the pre-check exists to prevent.

`l2_depth > 0` enables a **capability pre-check** at the gate: if the declared
depth is smaller than `criteria.min_l2_depth`, the probe fails on the first
poll with

```
l2: <venue> publishes <n> book level(s) per side but the strategy needs <m>
```

before a single frame is examined. Without it, a depth-1 venue would burn the
whole soak accepting frames whose second level is a zeroed slot.

`has_sequence_numbers` is currently **declared but unused** — see
[§18](#18-known-limitations-and-non-goals).

---

## 9. Probe criteria and the gate

```cpp
struct ProbeCriteria {
  uint64_t min_trades      = 100;
  uint64_t min_l2_frames   = 20;
  uint64_t min_l2_depth    = 2;      // match the deepest trigger "levels"
  int64_t  min_duration_ms = 2000;
  int64_t  timeout_ms      = 20000;
  uint32_t max_reconnects  = 0;
};
```

Presets:

| Preset | trades | l2 | depth | soak | timeout | reconnects |
|---|---|---|---|---|---|---|
| `Development()` | 100 | 20 | 2 | 2 s | 20 s | 0 |
| `Production()` | 300 | 100 | 2 | 60 s | 90 s | 1 |
| `Disabled()` | 0 | 0 | **0** | 0 | 0 | 0 |

`Disabled()` is what the parameterless `LiveOrchestrator::Start()` uses: the
engine arms on the first frame. It is *not* an unvalidated path — frames are
still checked once armed, and a fatal frame in the very first frames still
fails the start.

### Gate evaluation order (`EvaluateLocked`)

1. **Capability** — declared `l2_depth` < `min_l2_depth` → `Failed`.
2. **Fatal frame** — any fatal on a required channel → `Failed` immediately.
   Waiting for the deadline would only add latency to the same verdict.
3. **Reconnects** — `reconnects > max_reconnects` → `Failed`. A feed that keeps
   dropping is not healthy even if it eventually delivers the counts.
4. **Pass** — every required channel at or above its own minimum **and**
   `elapsed >= min_duration_ms` → `Ready`.
5. **Deadline** — `elapsed >= timeout_ms` → `Failed` with a shortfall message.

Until one of those fires, the state is `Probing`.

### Contract, restated as testable rules

- A channel passes only if `fatal == 0` **and** (`!required` or
  `delivered >= min_required`).
- A partial pass is a failure. Trades perfect + book malformed = `Failed`.
- A silent channel fails at the deadline, not silently.
- A transport error fails immediately.
- A clean end of stream before the minimums fails as
  `stream ended before the checks completed`; a clean end *after* the minimums
  keeps whatever verdict was already reached.

The gate never touches a socket, a clock or a log file. `now_ms` is passed in
by the caller, which is what makes every rule above testable with a loop and a
struct literal.

---

## 10. The report

`ProbeReport` carries one row per channel plus a verdict row. Valid after
`Start()` returns, on success **and** on failure; empty if `Start()` was never
called.

```
hyperliquid   trades  ok    (1204 msgs, 3 rejected, first: trade timestamp went backwards)
hyperliquid   l2      ok    (96 msgs, 0 rejected)
hyperliquid   gate    ok    (2013 ms)
```

A silent channel that never produced anything, so the deadline decided it:

```
bybit         trades  FAIL  (0 msgs, 0 rejected, needed 100)
bybit         l2      FAIL  (0 msgs, 0 rejected, needed 20)
bybit         gate    FAIL  (20011 ms)
  reason: not enough well-formed data after 20011 ms: trades 0/100 l2 0/20
```

A capability failure. Only reachable for a venue whose profile declares a depth
— an undeclared venue (`l2_depth == 0`) cannot fail this way, because the gate
has nothing to compare:

```
vendorx       l2      FAIL  (0 msgs, 0 rejected, needed 20)
vendorx       gate    FAIL  (3 ms)
  reason: l2: vendorx publishes 2 book level(s) per side but the strategy needs 20
```

Reading a row:

| Field | Meaning |
|---|---|
| `N msgs` | frames that passed validation (`delivered`) — the only ones the engine ever sees |
| `R rejected` | `recoverable + fatal` frames; a rejection never reaches the engine |
| `, <code>` | first **fatal** reason, when `fatal > 0` |
| `, first: <code>` | first **recoverable** reason, when there is no fatal |
| `, needed N` | the channel is required and delivered fewer than its minimum |

**A channel can print `FAIL` with clean frames.** `passed()` requires the
minimum, so a run that fails early shows `FAIL ... needed 100` on a channel
whose few frames were all perfect. That is intended: the row answers "did this
channel clear its bar", not "were its frames syntactically fine".

`ProbeReport::passed()` checks the gate state, not the rows. They agree for
required channels by construction.

---

## 11. Running-phase validation

Once armed, the router validates every frame again. This is not redundant:
after a reconnect or an API change, the feed can degrade without the process
restarting.

- Recoverable → dropped, `DroppedWhileRunning()` incremented. Nothing logs per
  frame; the count is reported at shutdown and is the number to look at when
  the engine reconciles badly against the venue's tape.
- Fatal → logged and delivered once through the error channel as
  `data integrity: <channel>: <code>`. The orchestrator treats it like a
  transport error: latch `stream_ended_`, drain, exit.

Escalation happens **once per router instance** (`escalated_`). A venue-wide
field rename produces one error, not one per frame.

Two counters must not be confused:

| Counter | Source | Meaning |
|---|---|---|
| `DroppedEvents()` | orchestrator queue overflow | engine too slow; **terminal** — `book_degraded_` latches and new entries stop |
| `DroppedWhileRunning()` | router | frames the validator refused; recoverable, book keeps last good snapshot |

---

## 12. Configuration reference

```json
"startup_probe": {
  "preset": "production",
  "min_trades": 300,
  "min_l2_frames": 100,
  "min_l2_depth": 2,
  "min_duration_ms": 60000,
  "timeout_ms": 90000,
  "max_reconnects": 1,
  "on_failure": "abort"
}
```

| Key | Type | Default (dev) | Notes |
|---|---|---|---|
| `preset` | `"development"` \| `"production"` | `development` | Applied first |
| `min_trades` | number ≥ 0 | 100 | Explicit keys override the preset |
| `min_l2_frames` | number ≥ 0 | 20 | |
| `min_l2_depth` | number ≥ 0 | 2 | Must match the deepest trigger `levels` |
| `min_duration_ms` | number ≥ 0 | 2000 | Must be ≤ `timeout_ms` |
| `timeout_ms` | number ≥ 0 | 20000 | Hard ceiling |
| `max_reconnects` | number ≥ 0 | 0 | Gaps tolerated during the soak |
| `on_failure` | `"abort"` \| `"warn"` | `abort` | See [§13](#13-failure-policy-and-exit-behaviour) |

The whole block is optional. Omitting it keeps `ProbeCriteria::Development()`
and `probe_abort_on_failure = true`: a config that says nothing is still
checked, just quickly.

### Validation (in `AppConfig::Validate`)

| Check | Why |
|---|---|
| `timeout_ms >= min_duration_ms` | A soak longer than its own deadline can never pass; the deadline fires first on every run and reads as "the venue is broken" |
| both `>= 0` | — |
| `min_l2_depth <= MAX_L2_DEPTH` (20) | A requirement deeper than the engine stores can never be met; every frame would be rejected as shallow |

Negative values are **rejected**, not clamped. `"min_trades": -1000` would
otherwise wrap through `uint64_t` and read as an astronomically high minimum.

Unknown `preset` or `on_failure` strings are config errors.

---

## 13. Failure policy and exit behaviour

`LiveOrchestrator::Start(criteria)`:

1. Builds the router and attaches it to the streamer (**before** `Start()`).
2. Starts the worker thread, then the streamer.
3. Polls the gate every 20 ms, logging progress every 5 s:
   `startup checks in progress (N ms): trades=A l2=B rejected=C`.
4. On `Failed`:
   - logs `startup checks FAILED after N ms: <reason>`
   - stops the streamer, requests shutdown, joins the worker
   - flushes the Redis publisher (the `SYSTEM_STARTUP` ping is queued before
     the checks run; without the flush it never reaches Redis)
   - **skips** the end-of-run flatten: nothing was armed, and closing at a
     price taken from a book the gate just rejected would book a phantom loss
   - returns `false`, leaving `PreflightReport()` readable.

`src/main.cpp` then:

- prints the report table to stdout,
- logs the reason,
- with `on_failure: "abort"` (default) throws
  `live mode: refusing to trade on a feed that failed its startup checks (...)`
  → exit code 1,
- with `on_failure: "warn"` logs a prominent warning and calls the ungated
  `Start()`, which is a **second, unchecked handshake** — exactly the gap the
  gate exists to remove. Opt-in only.

---

## 14. Operability

| Signal | Where | What it means |
|---|---|---|
| `starting market streamer (startup checks: trades>=… l2>=… for >=…ms, timeout …ms)` | `logs/engine.log` | Criteria in effect for this run |
| `startup checks in progress (N ms): trades=A l2=B rejected=C` | every 5 s while probing | Liveness. A one-minute soak is indistinguishable from a hang without it |
| `startup checks passed (N ms); engine armed` | router | Flip happened |
| `startup checks FAILED after N ms: <reason>` | orchestrator | Include the reason verbatim in bug reports |
| `data integrity: <channel>: <code>` | orchestrator | A **fatal** frame arrived while running; the run is stopping |
| `stopped (dropped=N rejected_frames=M)` | orchestrator | `dropped` = queue overflow (terminal); `rejected_frames` = router refusals |

The report table goes to stdout so it survives a redirected log and is visible
in a CI transcript.

---

## 15. Troubleshooting

| Report / log | Cause | Action |
|---|---|---|
| `l2: <venue> publishes N book level(s) per side but the strategy needs M` | The venue profile declares less depth than `startup_probe.min_l2_depth`. Fails before a single frame arrives | Subscribe deeper and raise `l2_depth`, or lower `min_l2_depth` and accept that the depth triggers starve |
| `trades: trade price <= 0` / `l2: book level price is not finite` | Parser bug or venue field change | Check the streamer's field names against the venue's current payload |
| `l2: bids are not strictly descending` | Mis-sorted book (parser bug or venue quirk) | Fix the parser; never relax this check — depth triggers index by position |
| `l2: book is shallower than the strategy needs` | The frame carried fewer levels than `min_l2_depth`. On Bybit this means the local book was still filling after a snapshot | Frequent occurrences point at the subscription or the reconciliation in [§6.4](#64-snapshotdelta-book-maintenance-producer-side), not at market thinness |
| `BybitDataStreamer: orderbook update gap (u=…); resyncing from a fresh snapshot` | A delta was missed, so the local book is quietly wrong. The streamer resets and re-subscribes by itself | Occasional is normal on a busy feed. Repeated occurrences mean frame loss — check the link and whether the process is CPU-starved |
| `BybitDataStreamer: order book still unusable after the resync grace period; forcing a reconnect…` | The re-subscribe did not replay a snapshot within the watchdog window | Normal once after a gap. Repeating with growing gaps means the venue is not delivering snapshots at all — check the subscription and venue status |
| L2 goes quiet during a live run with **no** validator error | Either the local book is one-sided (a resync in progress, suppressed rather than emitted) or deltas are arriving before any snapshot | Search the log for `update gap` and `delta before first snapshot`; a `snapshot` frame must arrive before a delta is applied |
| `not enough well-formed data after N ms: trades A/B l2 C/D` | Quiet instrument, wrong symbol, or every frame was rejected | Check the symbol first, then the venue subscription, then lower the minimums |
| `feed reconnected N time(s) during the checks (max M)` | Unstable link or subscription rejection | Fix the network, or raise `max_reconnects` deliberately |
| `transport error: …` | Handshake/auth/host failure | Check `venue.environment`, `allow_mainnet`, and the venue spec host |
| `stream ended before the checks completed` | The streamer closed cleanly before the minimums (short replay, rejected subscription) | Check the streamer's close reason |
| Engine starts, no entries | **Not validation.** Check `book_gap` / `book_degraded`, the prop guard, and whether the venue wallet stream has reported equity | See the orchestrator logs |

If a report row says `FAIL … needed N` with zero rejected frames, the channel
was simply silent — the venue accepted the subscription but produced nothing in
time.

---

## 16. Adding a venue

Checklist, in order. If step 6 requires an edit to `live_engine.cpp`, the seam
is in the wrong place and should be fixed before shipping.

1. **Streamer** implementing `IMarketStreamer`.
   - Emit `integrity::MakeBookGap(NowMillis())` on reconnect, unexpected close
     and transport error.
   - Track `first_open` so the initial handshake emits **no** gap.
   - Clear the running flag **before** stopping the socket, so a close you
     asked for does not look like a drop.
2. **Parser**.
   - Fill `bids[]` strictly descending, `asks[]` strictly ascending, starting at
     index 0, with the remainder zeroed.
   - Set `best_bid_price` / `best_ask_price` (and qty) consistently with the
     arrays.
   - Truncate at `MAX_L2_DEPTH`; record nothing else in the struct.
   - If the venue sends snapshot+delta rather than a full book, reconcile it in
     the streamer ([§6.4](#64-snapshotdelta-book-maintenance-producer-side)) and
     emit only complete snapshots. Never forward a delta as a snapshot.
3. **Profile** — add a case in `src/venue_profile.cpp` with the real
   `l2_depth` (post-truncation) and whether the venue sequences its book.
4. **Whitelist** — add the name to `AppConfig::Validate` and to the factory's
   dispatch, and remove any fall-through that would silently paper-trade a typo.
5. **Docs** — add the venue to the profile table in this file.
6. **Tests** — a `FakeStreamer`-style router test proving a malformed frame
   from this venue keeps the engine unarmed, plus (preferably) golden fixtures
   `raw payload → expected TradeEvent/L2Snapshot` with no network.
7. **Depth** — set `startup_probe.min_l2_depth` to match the deepest trigger
   `levels` in the shipped configs, and confirm the venue's subscription meets
   it.

---

## 17. Testing

```bash
cmake --build build --target engine_tests && ./build/engine_tests
```

```
./build/engine_tests "[router]"          # router end-to-end only
./build/engine_tests "*depth*"           # depth-rule cases
```

There is no CTest registration by design: the Catch2 binary is the single
runner, so its exit code is the verdict.

### Live feed diagnostic (Bybit)

`[.live]` starts with a dot, which Catch2 treats as **hidden**: a plain
`./build/engine_tests` skips it, so CI never needs a socket. It drives the
production `BybitDataStreamer`, `ProfileForVenue` and `StartupGate`, so a pass
means the same feed would arm the engine, and it is what the old `venue_doctor`
was trying to be.

**For "is this venue good right now" during operations you do not need it.** A
live `mokuren` run prints the same report before it arms and aborts on failure
(§13). The diagnostic exists for the assertions a normal run does not make —
the shallow-frame ratio, the gap count and time-of-book — which is why it is a
test and not a second program.

```
./build/engine_tests "[.live]"                   # ~10 s, needs network
```

It prints the usual report table plus a frame summary:

```
bybit         trades  ok    (61 msgs, 0 rejected)
bybit         l2      ok    (402 msgs, 0 rejected)
bybit         gate    ok    (10004 ms)
  frames: books=402 deep=402 shallow=0 gaps=0 trades=61 max_time_of_book_ms=41
```

| Counter | Healthy | What it catches |
|---|---|---|
| `deep` / `shallow` | `shallow` ≈ 0 | A delta forwarded as a snapshot: structurally valid, clears the depth check only when it happens to mention 20 levels, so `shallow` above ~5% means the local book is not being maintained. This is the only assertion that would also pass against `orderbook.1` |
| `gaps` | 0 | Reconnects and `update gap` resyncs during the window. Non-zero with `max_reconnects: 0` fails the gate, same as production |
| `max_time_of_book_ms` | tens of ms | Stalled frame processing, a throttled socket, or local clock skew |
| `trades` | any | The trades channel is alive at all |

A failure prints the report first, so the reason is in the output rather than
inferred from the assertion.

**What it does not prove:** that the local book matches the venue's. Depth and
ordering are self-consistency checks; a dropped delta that removed no level
leaves a book that is valid, complete-looking and stale. Cross-check by hand
occasionally:

```
curl -s 'https://api.bybit.com/v5/market/orderbook?category=linear&symbol=BTCUSDT&limit=50' | jq '.result.ts, .result.b[0], .result.a[0]'
```

Compare against the `LIVE L2` line in the log at the same moment: best bid/ask
should agree to within normal movement, and `ts` should be within a second.
CURL is already linked into `engine_tests`, so automating that comparison is a
change to the diagnostic alone.

Test conventions worth preserving:

- **Depth-1 fixtures by default.** `FastCriteria()` in both test files sets
  `min_l2_depth = 1` deliberately, because most cases are about structural
  rules; depth has its own cases. A test that changes this should say why.
- **Gate start comes from the same clock as the router.** `FeedRouter` stamps
  observations with `NowMillis()`; a gate constructed with a different `start_ms`
  fails instantly on the deadline. Tests use `StartMs() = NowMillis()`.
- **Test case names are global in Catch2.** Two translation units cannot share
  a name; the router-level reconnect case is tagged `[router]` and named
  differently from the gate-level one for this reason.
- **`FakeStreamer` is a real `IMarketStreamer`.** Router tests push frames
  through the production callback path, not around it.

Covered invariants (see the two test files):

- every fatal and recoverable rule, one case each, on struct literals;
- depth below requirement → fatal, and capability pre-check → `Failed` before
  any frame;
- partial pass (trades fine, book malformed) → `Failed`;
- silent channel → `Failed` at the deadline;
- reconnect → `Failed` under the default budget;
- transport error and early stream end → `Failed`;
- nothing forwards before `BeginRunning()`;
- the handoff emits exactly one book-gap and nothing else;
- fatal while running escalates once, recoverable while running is dropped and
  counted;
- snapshot/delta application, ordering, update-id gaps and truncation, off-line
  (`test/test_order_book.cpp`);
- the resync backoff ladder, its reset and its ceiling
  (`test/test_resync_watchdog.cpp`).

---

## 18. Known limitations and non-goals

| Limitation | Consequence | Notes |
|---|---|---|
| `has_sequence_numbers` unused | No seq-gap reconciliation. A silently dropped delta is not detected by this layer | Deliberate: "can be reconciled from seq" is not "timestamps are monotonic", and marking one out-of-order Bybit snapshot fatal would kill runs on reconnects |
| Book reconciliation lives outside this layer | `OrderBook` is shared, but nothing forces a new streamer to use it ([§6.4](#64-snapshotdelta-book-maintenance-producer-side)) | This layer validates frames, not sequences. A streamer that forwards a delta as a snapshot produces frames it accepts |
| Update-id continuity uses `u` only | A gap that skips a `u` value is caught; bybit's cross-stream `seq` is not checked | Adequate for a resync-on-gap policy, not a full sequencing guarantee |
| Replay decoder not wired yet | `.mokuren` files are not yet run through `ValidateTrade` / `ValidateL2` | Same rules, same reason codes; wiring it is the intended second call site |
| `tick_size` not verified against the venue | A config tick smaller than the venue's produces half-tick levels and silently wrong footprints; `Aggregator` rejects `tick <= 0` but cannot know the venue's real tick | Candidate for the same gate |
| `on_failure: "warn"` reconnects ungated | The second handshake is not checked | Opt-in, logged, never the default |
| No frames reach `OnTrade` during the probe | The Redis price ticker is silent for the soak duration (up to 60 s in production) | Acceptable, but a dashboard expecting a heartbeat during startup will look dead |
| Handoff window drops frames | Frames between `Ready` and `BeginRunning()` are discarded, not buffered | At most one poll interval (20 ms) |
| `FeedRouter` uses its own wall clock | The gate takes an injected clock; the router does not, so tests must build `start_ms` from `NowMillis()` | A wart; a clock parameter removes it |
| Report rows and gate verdict are separate queries | `ProbeReport::passed()` reflects the gate state; channel rows reflect their own minimums | They agree for required channels by construction |
| Not a strategy check | A clean feed says nothing about whether a signal is worth taking | Out of scope by design |
