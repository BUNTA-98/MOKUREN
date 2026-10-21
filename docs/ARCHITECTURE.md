# Mokuren Engine — Architecture & Developer Guide

> Status: revised against the implementation. Read from `src/main.cpp`,
> `src/factory.cpp`, `src/live_engine.cpp`, `src/mokuren.cpp`,
> `src/aggregator.cpp`, `include/trigger.hpp`,
> `include/market_data/format.hpp` and `test/test_filters.cpp`.
>
> All items previously marked **(verify)** have been resolved against the
> source. The TUI layout header is explicitly out of scope and was not reviewed.
>
> Intended use: pre-refactoring orientation. Section 1 gives the model, section 4
> lists what is worth fixing first, section 5 proposes an order.

---

## 1. High-Level Architecture & Data Flow

Mokuren is a single-process trading engine with three run modes that share the
same core pipeline:

- **Backtest** — replay historical bars/trades from an in-memory or file source.
- **Replay / paper** — feed recorded market data through the live code path, but
  execute against a simulated broker.
- **Live** — stream from a venue over WebSocket, execute through a venue adapter.

The central idea is that market data is normalised once, aggregated into bars
with order-flow metadata, evaluated by a strategy, vetoed by a filter chain,
sized and risk-checked, and finally handed to a broker. Everything downstream of
the `Aggregator` reasons about the same `Bar` / `MarketContext` objects, which is
what keeps backtest and live behaviour comparable.

### 1.1 Component overview

        ┌──────────────────────────────────────┐
        │ Data source                          │
        │  - IMarketStreamer (live WS)         │
        │  - replay reader (market_data fmt)   │
        └──────────────────┬───────────────────┘
                           │ TradeEvent / L2Snapshot
                           ▼
        ┌──────────────────────────────────────┐
        │ Aggregator                           │
        │  - OHLCV bars                        │
        │  - footprint / bid-ask volume        │
        │  - CVD, POC, VWAP                    │
        │  - L2 ring buffer                    │
        │  - session metrics                   │
        └──────────────────┬───────────────────┘
                           │ history + live bar + L2 + session
                           ▼
        ┌──────────────────────────────────────┐
        │ MarketContext (borrowed references)  │
        └──────────────────┬───────────────────┘
                           ▼
        ┌──────────────────────────────────────┐
        │ Strategy layer                       │
        │  ITrigger::EvaluateCandle/Tick       │
        │        ↓ TradeSignal                 │
        │  IFilter chain (veto only)           │
        └──────────────────┬───────────────────┘
                           ▼
        ┌──────────────────────────────────────┐
        │ Risk & trade management              │
        │  - RiskPcts / R-multiples            │
        │  - SL / TP / BE / trailing           │
        │  - PropGuard (daily/total DD)        │
        └──────────────────┬───────────────────┘
                           ▼
        ┌──────────────────────────────────────┐
        │ Execution                            │
        │  IBroker implementations             │
        │  (bybit, hyperliquid, paper/replay)  │
        └──────────────────┬───────────────────┘
                           ▼
        ┌──────────────────────────────────────┐
        │ Output & accounting                  │
        │  TradeRecord / TradeInfo / UIResult  │
        │  EngineState → TUI, Redis, logs      │
        └──────────────────────────────────────┘

### 1.2 Start-to-finish flow (live)

1. **Entry point** — `src/main.cpp` parses `--live`, `--config=` and `--debug`,
   initialises the async logger, then either opens the TUI (`UIManager::Run`)
   or calls `RunLive(config_path)`. SIGINT only raises an atomic flag; the live
   loop polls it.
2. **Config and composition** — `RunLive` reads the JSON, collapses grid objects
   with `FlattenGrids`, calls `AppConfig::Load`, then
   `StrategyFactory::Build(cfg, /*live_mode=*/true)`. The factory is the single
   composition root: two `Aggregator`s (base and macro interval), a
   `PaperTrader`, a venue `Spec` → `BybitDataStreamer` and optionally
   `BybitBroker`, `PositionManager`, triggers, filters, `AlphaEngine`,
   `PositionSizer`, `RiskManager` plus its locks, and the R-multiple ladder.
3. **Orchestrator** — `main.cpp` moves `engine.streamer` out and constructs
   `LiveOrchestrator(engine, streamer, cfg.sl_pct, cfg.tp_pct, "127.0.0.1",
   6379, cfg.symbol, cfg.prop)`. Risk parameters and the prop rules travel with
   the config, not with the broker.
4. **Start** — `Start()` resets the prop guard and its latches, wires four
   streamer callbacks (trade, L2, finished, error), publishes `SYSTEM_STARTUP`,
   spawns one worker thread, then starts the streamer.
5. **Ingress (streamer thread)** — `OnTrade` throttles a Redis ticker publish and
   enqueues; `OnL2` enqueues. Both go through `Push`, a bounded deque
   (`kMaxQueueDepth`) guarded by `mutex_`. Overflow drops the newest event and
   latches `book_degraded_` for the rest of the run.
6. **Worker loop (one thread)** — `Pop` waits on a condition variable.
   - **L2 event**: `ProcessL2` on both aggregators → skip if `live_bar.close ==
     0.0` → `alpha->Evaluate(ctx, false)` → execute at `best_ask_price` /
     `best_bid_price`.
   - **Trade event**: `CheckPropGuard(price, ts)` → `pos_manager->Update` →
     `broker->CheckRisk` → `ProcessTrade` on both aggregators → skip if
     `close == 0.0` → `alpha->Evaluate(ctx, candle_finished)` → execute at the
     trade price.
7. **Signal funnel** — `execute_signal` rejects `NONE`, refuses new risk while
   `book_degraded_`, asks `prop_guard_.allow_new_entry()`, validates the stop,
   sets `entry_price = exec_price`, runs `PositionSizer`, then `RiskManager`,
   re-validates the stop, derives a missing take profit, and calls
   `broker->ProcessSignal`. A transition to an open position publishes
   `TRADE_OPEN`.
8. **Exit path** — on every print, `PositionManager` (break-even, trailing,
   scale-out) and `IBroker::CheckRisk` (stop, target) run *before* the print is
   folded into the bar.
9. **Shutdown** — a clean end of stream or a socket error sets `stream_ended_`
   and requests shutdown; `main` notices within 100 ms and calls `Stop()`.
   `Stop()` publishes `SYSTEM_SHUTDOWN`, stops the streamer, drains the queue,
   joins the worker, flushes the last candle, force-closes any open position and
   flushes Redis.

### 1.3 Start-to-finish flow (backtest / WFA)

`src/mokuren.cpp` drives every non-live run. The shape mirrors live, with these
differences:

- **Data**: `DataManager::LoadAllTrades` / `LoadAllL2Snapshots` load the whole
  dataset into RAM; L2 is caught up with
  `while (l2.timestamp <= trade.timestamp)` before each print, so tick and
  candle evaluation interleave exactly as they do in the live worker.
- **Execution**: always `PaperTrader`. The factory constructs a venue adapter
  only when `live_mode == true`.
- **Pre-flight**: `ValidateRunConfig` (`AppConfig::Load`, `ResolveRiskPcts`,
  `ValidateTradeManagement`) runs once on the driver thread, so a config error
  aborts before the dataset is loaded instead of surfacing as "no trades".
- **Permutations**: `GridScanner::GenerateGrid` produces `RunConfig`s;
  `max_cores` workers pull indices from an atomic counter, each building its own
  `EngineInstance`. Results go through `on_result` / `on_progress` under a UI
  mutex and are appended to `runs/scan_<timestamp>/results.csv`.
- **WFA**: `DataManager::GenerateWFAWindows` slices in/out-of-sample ranges; the
  in-sample grid runs in parallel, the single best-by-profit config is re-run on
  the out-of-sample slice, and its trades are concatenated into one aggregate
  `TestResult`.

Because the strategy only sees `MarketContext`, a backtest result is meaningful
only if the replay path produces the same bars, footprint and L2 history as the
live path. Any divergence there is a correctness bug rather than a feature
difference — and the two paths do diverge today (see P0).

### 1.4 Order lifecycle

    ITrigger::EvaluateCandle / EvaluateTick          (include/trigger.hpp)
        │  TradeSignal{direction, entry, stop, target, volume}
        ▼
    [live only] book_degraded_ gate                  (src/live_engine.cpp)
    [all paths] PropGuard::allow_new_entry()         (include/prop_guard.hpp)
        │
        ▼
    stop sanity check                                (SignalExecutor)
        ▼
    PositionSizer::CalculateSize
        │
        ▼
    RiskManager::Evaluate    ── SinglePositionLock, MaxStopDistanceLock,
        │                       MaxLeverageLock, AntiRevengeLock
        ▼
    stop re-validation after sizing and risk        (SignalExecutor)
        │
        ▼
    take-profit derivation (only when the strategy left it at 0)
        │
        ▼
    IBroker::ProcessSignal(signal, exec_price, ts)
        │
        ▼
    per print: PositionManager::Update → IBroker::CheckRisk
        │
        ├─ stop / target / scale-out   → TradeRecord(exit_reason)
        ├─ shutdown                    → ClosePartial("shutdown") or
        │                                PaperTrader::CloseOpenPositionAtEnd
        └─ prop breach                 → ClosePartial("prop_guard")

Two ordering guarantees are worth knowing before touching any of this:

- **Exits run before aggregation.** `CheckRisk` sees the price of the current
  print before `ProcessTrade` folds that print into the bar, so a stop is
  evaluated against exactly the price the strategy will later see.
- **Candle evaluation runs on every print, not only at bar close.**
  `ProcessTrade` returns `candle_finished`, and that flag is passed to
  `alpha->Evaluate`, but the call happens for every trade (and for every L2
  snapshot on the tick path). Most built-in triggers read `context.history.back()`
  — the last *closed* bar — so they re-evaluate the same bar until a position
  exists. See P0 "no fired latch".

### 1.5 Threads, ownership, and shared surfaces

| Thread | Created by | Responsibilities |
|---|---|---|
| main | OS | CLI parsing, logger init; live: 100 ms poll of `StreamEnded()`; backtest: owns `UIManager`. |
| streamer | `ixwebsocket` via `IMarketStreamer::Start` | Networking; invokes trade / L2 / finished / error callbacks. |
| worker (exactly one) | `LiveOrchestrator::Start` | Drain queue, prop guard, aggregation, strategy, filters, sizing, risk, broker calls, Redis events. |
| redis publisher | `RedisPublisher` | Bounded queue → pub/sub and stream; background flush. |
| spdlog async | `InitLogger` | 8192-entry queue, one worker thread, 10 MB × 5 rotation. |
| grid / WFA workers | `RunGridSearch`, `RunWFA` | One `EngineInstance` per permutation, up to `max_cores`. |

Guards:

- `queue_`, `shutdown_`, `stream_error_` — `LiveOrchestrator::mutex_`.
- `running_`, `dropped_`, `book_degraded_`, `stream_ended_`, `g_quit` — atomics.
- `live_bar_`, `htf_live_bar_`, `prop_guard_`, `last_prop_decision_` — worker
  only. `Stop()` touches `live_bar_` only after `join()`; that ordering is what
  makes the end-of-run flush safe.
- `EngineInstance` is moved by value but owns everything through `unique_ptr`,
  and every internal cross-reference (`PipelineStrategy` → trigger/filters,
  `PositionSizer` and the locks → active broker, `AlphaEngine` → pipeline) is a
  raw pointer to a heap object whose address does not change on move. Moving the
  instance is safe; copying it is not possible.
- `EngineState` / `UiState` are written by backtest workers and read by the TUI
  (section 3.6).

---

## 2. Directory & Module Breakdown

### 2.1 Top-level layout

| Path | Responsibility |
|---|---|
| `include/` | Contracts, core data types, header-only helpers. |
| `src/` | Engine implementations, entry points, venue adapters, logging, risk. |
| `tui/` | Dashboard state and rendering contracts. |
| `test/` | Catch2 tests (currently filters only). |
| `docs/` | Developer documentation (this file, plus the market-data spec). |
| `config/`, build scripts | Not reviewed. |

Main translation units:

| File | Role |
|---|---|
| `src/main.cpp` | CLI, logger init, TUI vs `RunLive`, SIGINT handling, 100 ms poll. |
| `src/factory.cpp` | Composition root: venue spec, broker/streamer, managers, risk modules, R ladder. |
| `src/live_engine.cpp` | `LiveOrchestrator`: ingress queue, worker loop, prop guard, Redis events, shutdown. |
| `src/mokuren.cpp` | `RunGridSearch`, `RunWFA`, `ReplaySingleRun`, `ReplayWFA`, shared signal helpers. |
| `src/aggregator.cpp` | Bar construction, sliding footprint grid, POC, session VWAP. |

### 2.2 Core data & market model

| File | Purpose |
|---|---|
| `include/trade_event.hpp` | `TradeEvent` (timestamp, price, qty, aggressor side) and `L2Snapshot` / `L2Level` with fixed arrays for depth. |
| `include/aggregator.hpp` | `Bar`, `PriceLevel`, `SessionMetrics`, `L2RingBuffer`, `TradeSignal`, `SignalDirection`, and the `Aggregator` class. This is the de-facto core types header. |
| `src/aggregator.cpp` | Bar construction, footprint packing, POC/CVD/VWAP, session reset, L2 push. |
| `include/market_context.hpp` | Immutable-ish bundle passed to strategies: history, live bar, HTF bar, session metrics, latest L2, L2 history. |
| `include/timestamp_utils.hpp` | Heuristic normalisation of ns/µs/ms epoch values to milliseconds. |
| `include/market_data/format.hpp` | On-disk binary format: file header, block header, index entries, trade/book records, codecs, granularity. |

### 2.3 Strategy & filtering

| File | Purpose |
|---|---|
| `include/trigger.hpp` | `ITrigger` interface. Candle evaluation is mandatory; tick evaluation defaults to no signal. |
| `include/filter.hpp` | `IFilter` interface and concrete veto filters: minimum volume, volatility, CVD divergence, order-book imbalance. |
| `include/trade_management.hpp` | Config-boundary resolvers: `ResolveRiskPcts`, `ResolveTpPct`, `ResolveLevelPct`. Canonical R keys win over legacy absolute keys; a missing level is a hard error. |

### 2.4 Execution, venues, and streaming

| File | Purpose |
|---|---|
| `include/ibroker.hpp` | Broker-facing records (`StopEvent`, `TradeRecord`) and the `IBroker` interface. |
| `src/bybit_broker.cpp` | Bybit adapter. |
| `src/hyperliquid_broker.cpp` | Hyperliquid adapter. |
| `include/live_streamer.hpp` | `IMarketStreamer` with trade/L2/finished/error callbacks and explicit start/stop semantics. |
| `src/hyperliquid_streamer.cpp` | Hyperliquid market-data streamer. |
| `include/venue_spec.hpp`, `src/venue_spec.cpp` | Venue environment (`Mainnet`/`Testnet`/`Demo`), category, hosts, and a human-readable manifest. |
| `include/config_parser.hpp` | `AppConfig` (venue spec, credentials, risk, prop rules, trade management), `TradeManagementConfig`, `ComponentConfig`, `RunConfig`, `GridParam`, `GridScanner`, `FlattenGrids`. |

### 2.5 Risk & accounting

| File | Purpose |
|---|---|
| `include/prop_guard.hpp`, `src/prop_guard.cpp` | Funded-account survival rules: daily and total drawdown, warning fraction, halt states, flatten transition. |
| `include/engine_state.hpp` | Shared run state: atomics, status text, result vector, WFA/OOS config, `TradeInfo`, `UIResult`. |
| `include/wfa_window.hpp` | In-sample / out-of-sample window boundaries. |
| `include/mokuren.hpp` | Umbrella header and `AdvancedMetrics` (Sharpe, Sortino, Monte-Carlo drawdown). |

### 2.6 Presentation & observability

| File | Purpose |
|---|---|
| `tui/tui_dashboard.hpp` | `UiState` with a consistent snapshot (ticks, markers, symbol, monotonic sequence) and `TradeMarker` overlay data. |
| `include/ui_pages.hpp` | `UIPage` base: render, input, and global-hotkey blocking. |
| `include/ui_menu.hpp` | `PageConfig`: JSON-backed menu/editor page with category and inline-edit states. |
| `include/ui_theme.hpp` | Colour palette and semantic styles (bid/ask, POC, imbalance, heatmap). |
| `include/ui_pagelayout.hpp` | Geometry for header/footer/body regions. |
| `include/logger.hpp`, `src/logger.cpp` | Async rotating logger (`logs/engine.log`, 10 MB × 5, 8192-entry queue). |
| `include/live_debug.hpp` | Separate trace-level `live_debug.log` behind a global atomic switch, off by default. |
| `include/redis_publisher.hpp` | Bounded-queue Redis publisher on a background thread. |

---

## 3. Key Classes & Interactions

### 3.1 `Aggregator` — the market-data kernel

Responsibilities:

- Convert `TradeEvent` into OHLCV bars and footprint levels.
- Maintain cumulative delta, POC, bar VWAP, and session VWAP/volume.
- Own the L2 ring buffer and expose age-indexed history.
- Validate `interval > 0` and `tick > 0` at construction.

Important invariants (read from `src/aggregator.cpp`):

- `GetOrAddLevel` floors the price to the tick grid and anchors `base_price` at
  the centre of the `MAX_GRID_LEVELS` window on first touch. The "first touch"
  test is `max_touched_idx_ < min_touched_idx_` — the empty-span sentinel, not
  `active_levels`.
- A print outside the window slides it: surviving levels are `memmove`d by whole
  ticks, departing levels are subtracted from `active_levels`, and the touched
  span is remapped and clamped. A jump larger than the window clears the grid
  and re-anchors. `GetOrAddLevel` therefore returns non-null for every finite
  price. The earlier out-of-bounds concern is resolved; the cost is a
  `memmove` of the full level array per sliding print (P2).
- `ProcessTrade` updates session VWAP first, then closes any number of elapsed
  intervals, then folds the print into the current bar. Consequence: the print
  that crosses a boundary belongs to the *new* bar. Bars are stamped by interval
  start, not by the print that created them.
- An interval with no prints is carried forward from `history.back().close`, so
  ATR/SMA/RVOL and close-based triggers never see `open == 0`.
- On close: `AnalyzeCandle` (POC over the touched span) → `PackFootprint`
  (reserves `active_levels`, keeps only levels with volume) → `history.push_back`
  → `ResetBar` → `ResetWorkspace` → re-attach `last_l2`.
- `FlushLastCandle` pushes only when `total_volume > 0`, so a shutdown without a
  print never appends an empty bar.
- `ProcessL2` only pushes into the ring buffer. `Bar::last_l2` is refreshed on
  trades, so a bar carries the book at its last print, not at its close. Use
  `MarketContext::latest_l2` when the current book matters.
- `L2RingBuffer::get_history(age)` returns a pointer into the ring, invalidated
  by the next `push`.

Callers: the orchestrator feeds trades and L2 snapshots; strategies never call
the aggregator directly.

### 3.2 `MarketContext` — the strategy boundary

`MarketContext` is a non-owning view:

    const std::vector<Bar>& history;
    const Bar&               live_bar;
    const Bar&               htf_bar;
    SessionMetrics           session;
    const L2Snapshot&        latest_l2;
    const L2RingBuffer&      l2_history;

This avoids copying the (large) L2 snapshot per tick, but it creates a strict
lifetime contract: a context is valid only while the referenced objects are
alive and not mutated by a ring-buffer push. Strategies and filters must treat
it as ephemeral.

### 3.3 Strategy layer

    ITrigger
      ├─ EvaluateCandle(context) -> TradeSignal   (required)
      └─ EvaluateTick(context)   -> TradeSignal   (optional, default NONE)

    IFilter
      └─ AllowTrade(context, intended_signal) -> bool

Interaction order:

1. Trigger proposes a direction, entry, stop, target, and volume.
2. Each filter independently vetoes or allows. Filters are not modifiers; they
   cannot resize or re-price a signal.
3. Only an allowed signal reaches risk and execution.

Concrete filters currently bundled in `filter.hpp`:

- `MinVolumeFilter` — requires recent bar volume.
- `VolatilityFilter` — requires a minimum average range over a period.
- `CVDDivergenceFilter` — compares price change against summed CVD.
- `OrderbookImbalanceFilter` — requires a bid/ask quantity ratio in the signal
  direction.

### 3.4 Trade management & risk

`RiskPcts` is the single resolution point for the risk block:

- `sl_pct` is 1R and canonical.
- `tp` is resolved from `tp_r` (multiple of 1R) or legacy `tp_pct`.
- Break-even and trailing levels are resolved as fractions of 1R through
  `ResolveLevelPct`.

`PropGuard` is evaluated on every L2 snapshot and every trade print, on live and
backtest alike (see P0):

- `Unarmed` — no usable equity, refuse entries.
- `Normal` — trading allowed.
- `Warning` — no new entries, manage open positions.
- `DailyHalted` — no new entries until the day rolls over.
- `TotalHalted` — no new entries for the process lifetime.

`should_flatten` is true only on the transition into a breach, so the
orchestrator can flatten once without re-triggering every tick.

### 3.5 Execution & venue adapters

`IBroker` implementations (`BybitBroker`, `HyperliquidBroker`) are selected by
`AppConfig::exchange` in `StrategyFactory::Build`. They produce `TradeRecord`
objects containing entry/exit times, direction, prices, net profit, exit reason,
and the ordered list of `StopEvent`s.

`BybitBroker` is the only adapter that can trade. It fetches instrument rules
and reconciles an existing position at construction, attaches the stop and
target to the entry order, and leaves `CheckRisk` empty on purpose: break-even,
trailing and scale-out are `PositionManager`'s job, and running a second ladder
in the adapter would move the same stop twice. `GetBalance()` is served from the
private `wallet` push (`totalEquity`), so it is mark-to-market and includes open
P&L — and it is `0.0` until that frame arrives.

`HyperliquidBroker` is a stub: `SignOrderEIP712` returns an empty signature and
`ProcessSignal` / `UpdateStopLoss` / `ClosePartial` only log what they *would*
send. It cannot trade, and it hardcodes asset index 0 / coin `"BTC"`.

`venue::Spec` captures environment, category, symbol, and the REST/WS hosts.
`Manifest(spec)` provides a stable string for logs and audit.

### 3.6 Orchestration & shared state

`EngineState` is the shared control/result surface. It is a plain struct, not a
singleton: ownership is by the caller (UIManager / workers), and every holder
of a reference can mutate it.

- atomics: `is_running`, `is_finished`, `current_permutation`,
  `total_permutations`.
- mutex-protected: `status_text`, `top_results`.
- `UIResult` carries parameters, the full JSON config, net profit, win rate,
  trade count, drawdown, and TP/BE/SL percentages.

The TUI and Redis publisher read from this state. `UiState` in
`tui/tui_dashboard.hpp` is a separate, lock-protected snapshot for the chart
(ticks and trade markers), not the same object as `EngineState`.

### 3.7 Who calls whom (summary)

| Caller | Callee | Data |
|---|---|---|
| Streamer / replay | orchestrator callbacks | `TradeEvent`, `L2Snapshot` |
| Orchestrator | `Aggregator::ProcessTrade` | trade → live `Bar` |
| Orchestrator | `Aggregator::ProcessL2` | snapshot → ring buffer |
| Orchestrator | `ITrigger::EvaluateCandle/Tick` | `MarketContext` → `TradeSignal` |
| Orchestrator | `IFilter::AllowTrade` | veto decision |
| Orchestrator | `PropGuard::Observe` | equity → `PropDecision` |
| Orchestrator | `IBroker` | order / modify / close |
| Orchestrator | `EngineState` | status and results |
| TUI / Redis | `EngineState`, `UiState` | read-only snapshots |
| Config loader | `venue::Spec`, risk resolvers | typed and raw config |

The important boundary is: **strategy code never talks to a broker or an
exchange; execution code never talks to an aggregator.** The orchestrator is the
only component allowed to touch both, which is exactly why its loop is the place
every divergence starts.

### 3.8 `LiveOrchestrator` — the live event loop

`src/live_engine.cpp` is the only place where market data, account limits and
execution meet:

- **Ingress**: `Push(Event&&)` appends `{TRADE|L2, union}` to a bounded deque.
  The cap is a safety valve, not a tuning knob: dropping an L2 snapshot
  invalidates every depth-derived trigger, so overflow latches `book_degraded_`
  and `execute_signal` refuses new entries for the rest of the run. Existing
  positions keep their venue-side stop and still exit through `CheckRisk`.
- **Egress**: one worker thread, no second consumer. The aggregators,
  `PositionManager`, the broker cache and the prop guard are all single-threaded
  and are only touched there.
- **Account model**: `CheckPropGuard` builds equity per run mode. Venue path:
  `broker->GetBalance()` is mark-to-market and already includes open P&L. Paper
  path: closed balance plus `(price - entry) * volume * dir`, because
  `PaperTrader`'s balance only moves when a trade closes.
- **Events**: `TRADE_OPEN`, `POSITION_CLOSED`, `PROP_HALT`, `SYSTEM_STARTUP`,
  `SYSTEM_SHUTDOWN`, plus a ~1 s throttled ticker. All JSON, all on the
  publisher thread.
- **Shutdown**: `RequestShutdown` is safe from the streamer thread (it notifies,
  never joins). `Stop()` joins first, then flushes the bar and force-closes, so
  the final close cannot race the worker.

### 3.9 Where the signal funnel actually lives

The same five-step funnel (validate stop → size → risk modules → derive TP →
submit) is written out in five places:

| Site | File | Funnel | Prop guard | Venue |
|---|---|---|---|---|
| live worker | `src/live_engine.cpp` | `SignalExecutor` | yes (L2 + trade) | active broker |
| grid scan | `src/grid_runner.cpp` | `SignalExecutor` | yes (L2 + trade) | PaperTrader |
| WFA in-sample | `src/wfa_runner.cpp` | `SignalExecutor` | yes (L2 + trade) | PaperTrader |
| WFA out-of-sample | `src/wfa_runner.cpp` | `SignalExecutor` | yes (L2 + trade) | PaperTrader |
| replay (single) | `ReplayManager::Run` | `SignalExecutor` | yes (L2 + trade) | PaperTrader |
| replay (WFA) | `src/wfa_runner.cpp` | `SignalExecutor` | flatten only, flag discarded | PaperTrader |

This table is the most useful artefact in the document: every site now runs the
same funnel and the same account guard, so a change to sizing, stop validation,
take-profit derivation or account limits is made once. The remaining asymmetry
is the WFA replay, which flattens on a breach but does not report it.

---

## 4. Current Pain Points

### P0 — cross-path correctness

- **The execution funnel has five copies, and live is the hand-written one.**
  `PrepareSignalRisk` and `FinalizeTakeProfit` are file-static in
  `src/mokuren.cpp`, so `LiveOrchestrator::WorkerLoop` cannot call them and
  reimplements both inline. They have already diverged: live re-validates the
  stop **after** sizing and the risk firewall, the backtest helpers validate
  once, before sizing. A risk module that moves a stop to the wrong side is
  caught live and silently accepted in the scan/WFA. Fix: one `SignalExecutor`
  (or at minimum move the helpers into a shared header) used by every path.

- ~~`PropGuard` is live-only.~~ **Resolved.** Every driver constructs a guard
  per permutation/run (`src/grid_runner.cpp`, `src/wfa_runner.cpp`,
  `ReplayManager::Run`), seeds `initial_balance` from
  `PaperTrader::kInitialBalance`, and refuses entries through
  `allow_new_entry()`. The `10000.0` literals in the WFA aggregation and in the
  Monte-Carlo shuffle now read that constant. `TestResult::prop_breached` and
  the `PropBreached` CSV column carry the outcome out of the run. What is still
  missing is the *decision*: WFA selects on raw profit, so a breached winner is
  flagged but not rejected automatically, and 64-bit replay drops the flag.

- ~~The prop guard is only observed on trade prints.~~ **Resolved.**
  `CheckPropGuard` now runs in the live `L2` branch as well, and every backtest
  driver observes on both L2 snapshots and trade prints, so a floating loss
  that crosses a limit on a quiet book is caught on every path. A full
  reduction through `PaperTrader::ClosePartial` also books the win/loss ledger,
  so a prop_guard flatten is counted rather than silently dropped from
  `GetTotalTrades()` / `GetWinrate()`.

- **No "fired" latch per bar.** A trigger that reads `history.back()` returns the
  same signal for every print in the following bar until a position is open.
  That is only safe while `SinglePositionLock` is configured; without it, the
  sizer and the broker decide how many repeats become positions. Either dedupe
  in the funnel or make the lock mandatory.

- **`Aggregator` carries timeline state with no reset API.** `next_close_time`,
  `current_session_day_`, `session_vol_` and `session_price_vol_` are members and
  nothing in `include/aggregator.hpp` clears them. Feeding a second dataset
  through the same instance continues the first timeline: bars fast-forward
  through empty intervals and the session VWAP does not restart. Add `Reset()`
  or document one instance per run.

- **`ResetWorkspace` and the empty-span sentinel are in sync.** `GetOrAddLevel`
  decides "first touch" from `max_touched_idx_ < min_touched_idx_`, not from
  `active_levels`. `ResetWorkspace` clears only the touched span and then
  restores the sentinel (`min_touched_idx_ = MAX_GRID_LEVELS;
  max_touched_idx_ = -1;`), so the next candle starts from an empty span. No
  bug here; the invariant is worth a comment in the header.

- **`MacroTrendFilter` cannot see the macro timeframe.** `MarketContext` exposes
  exactly one higher-timeframe `Bar` and no HTF history, so the filter falls back
  to `context.history` with a default lookback of 1440 base bars. The
  `htf_aggregator` is built and fed, but its history never reaches a strategy.
  Expose an HTF history view or delete the second aggregator.

- **Two independent depth ceilings.** The on-disk format defines
  `kMaxBookDepth = 20` in `include/market_data/format.hpp`; the engine uses
  `MAX_L2_DEPTH` (referenced from `include/trigger.hpp`, defined alongside the
  `L2Snapshot` arrays in `include/trade_event.hpp`). Nothing asserts that they
  agree, so a valid file can be read into a snapshot type that cannot hold it.

### P1 — duplication and asymmetry

- **Four tolerant JSON readers.** `AppConfig` (`SafeBool` / `SafeNumber` /
  `RequireNumber`), `src/factory.cpp` (`IsDisabled` / `ParamNumber` /
  `ParamBool`), `src/mokuren.cpp` (`read_enabled` / `read_number`) and
  `GridScanner::FindParams`. Each accepts a slightly different set of spellings;
  the factory comments are a history of bugs caused by exactly that. Any new
  validation has to be added in four places.

- **The R-multiple ladder is armed twice, by two derivations.**
  `StrategyFactory::Build` divides each pct by `sl_pct`; `RunGridSearch`,
  `RunWFA` and `ReplayWFA` re-arm it via `ResolveLevelPct() / sl_pct`. Only
  comments claim the two agree. One test that feeds the same JSON through both
  would settle it.

- **Filters fail open on missing data.** `OrderbookImbalanceFilter` returns true
  when the relevant side's quantity is 0, `VwapTrendFilter` returns true when
  `vwap == 0`, `POCTrendFilter` returns true when `poc_price == 0`. A
  disconnected or empty book therefore *permits* entries, while
  `SpoofHunterTrigger` fails closed (`timestamp == 0` → no signal). Pick one
  policy and apply it everywhere.

- **`is_buyer_maker` in the engine, `Aggressor` in the format.**
  `include/market_data/format.hpp` argues explicitly that a venue-specific
  boolean inverts footprint, delta, POC and every imbalance threshold.
  `TradeEvent` still uses that boolean. Any adapter that mislabels it inverts
  CVD and the imbalance filters silently.

- **Replay was consolidated for one path only.** `ReplaySingleRun` delegates to
  `ReplayManager::Run` ("cannot drift from it again"), but `ReplayWFA` keeps its
  own inline copy of the same loop and funnel. `ReplayManager::Run` itself is
  the sixth funnel copy: it validates the stop before sizing but not after the
  risk firewall, and it re-derives the take profit from `cfg.tp_pct /
  cfg.sl_pct` rather than calling `FinalizeTakeProfit`.

- **Three drawdown definitions.** `PaperTrader::GetMaxDrawdown`, the hand-rolled
  loop in `RunWFA`, and `PropGuard`'s daily/total fractions. They are allowed to
  disagree, and the WFA number is not the one the account will be measured by.

- **Hardcoded infrastructure in the live path.** `main.cpp` passes
  `127.0.0.1:6379`, and the publisher's channel/stream names are literals chosen
  in `main.cpp` (`"mokuren:events"` in `LiveOrchestrator`'s constructor) and in
  `RedisPublisher`. It is configuration that never reaches `AppConfig`.

- **Sizing and risk depend on a silent prerequisite.** On the venue path the
  sizer reads exchange equity, which is 0.0 until the private wallet stream
  lands; `PositionSizer` then yields no volume and `MaxLeverageLock` rejects
  every signal. One warning is logged at start, but there is no explicit
  "armed" state, so a run that never receives a wallet frame looks exactly like
  a strategy that found no setups.

- **Streamer reconnect behaviour is inconsistent.** `BybitBroker`'s private
  socket and `HyperliquidDataStreamer` call `enableAutomaticReconnection()`;
  `BybitDataStreamer` and `BinanceDataStreamer` do not. A dropped public socket
  on those two ends the run via `EmitError`, while the private state feed
  recovers on its own. Pick one policy per streamer and document it.

- **`BybitBroker::ClosePartial` reduces local size before the REST call.** The
  optimistic decrement is deliberate (a repeated flatten must not send a second
  order), but a rejected close leaves `position_size_` smaller than the venue's
  position. The next `HasOpenPosition()` then disagrees with the exchange until
  the next `position` push corrects it.

- **`PositionManager` re-sends the trailing stop every tick.** The break-even
  branch latches `stop_moved_to_be_` on success, but the trailing branch only
  records `last_stop_attempt_ms_` on failure. Once trailing is active, every
  tick recomputes the same trail and calls `UpdateStopLoss` again. Idempotent on
  the venue, but it is a REST call per tick.

- **`HyperliquidBroker` is not a working adapter.** Signing is a stub, so every
  order path only logs. The factory already refuses a live Hyperliquid run for
  lack of a streamer, but the adapter itself is also incomplete; the doc should
  not imply otherwise.

### P1b — structural coupling (inferred)

6. **`SignalDirection` lives in `aggregator.hpp`, but is used by `ibroker.hpp`,
   `filter.hpp`, and trade records.** Every execution and strategy header
   therefore depends on the aggregator header, which also drags in `Bar`,
   `PriceLevel`, and the ring buffer. This is the clearest include-gravity
   problem in the codebase. Extract core enums and PODs into a small
   `core_types.hpp` / `trade_types.hpp`. (`include/ibroker.hpp` includes
   `aggregator.hpp` for exactly this reason.)

7. **Three config sources of truth.**
   - `AppConfig` (JSON keys, legacy environment bool).
   - `trade_management.hpp` resolvers reading raw `nlohmann::json`.
   - `venue::Env` tri-state (`Mainnet`/`Testnet`/`Demo`).
   The legacy bool cannot represent `Demo`, and the risk resolvers duplicate
   precedence rules already implemented in `AppConfig::Load` (`sl_pct` vs
   `sl_price_pct`). This is a correctness hazard, not just duplication.

8. **No explicit order-management layer.** Trade lifecycle data is split across
   `TradeInfo` (engine/UI), `TradeRecord` (broker), and `StopEvent`. Two
   representations of the same trade invite divergence. A single
   `Position`/`Trade` state machine owned by an OMS would make stop/TP/BE logic
   testable without a venue.

9. **`EngineState` is shared mutable state.** Atomics plus a mutex plus a result
   vector are handed directly to the UI. It is not a singleton, but every
   caller that holds a reference can mutate it. Parallel WFA runs, multiple
   symbols, or multiple engines in one process become difficult. Introduce a
   result sink interface and immutable snapshots.

10. **Stringly typed venue selection.** `AppConfig::exchange` is a string;
    `venue::Spec` models Bybit's three environments but Hyperliquid has a
    different model. A common `IVenue`/factory with per-venue environment
    mapping would stop venue assumptions leaking into the core.

11. **Two logging stacks.** The async `logger.hpp` and the separate
    `live_debug.hpp` logger have independent configuration and flush policies.
    Debug logging should be a level/sink of the main logger, not a second
    subsystem.

12. **UI state duplication.** `EngineState` and `UiState` both hold run state.
    The TUI reads both, and `UIResult` embeds the full JSON config per result.
    With many permutations this is a significant memory and copy cost.

### P2 — performance and maintainability

13. **Recomputed indicators.** `CVDDivergenceFilter` sums over a lookback on
    every call; `VolatilityFilter` scans a window. Multiple triggers/filters
    repeat similar scans over the same history. Precompute per-bar indicators
    once, or cache them in the context.

14. **`PackFootprint` scans gaps.** It reserves `active_levels` but iterates the
    whole touched range, including empty levels. Bounded by the grid, but worth
    measuring for wide ranges.

15. **Large `L2Snapshot` copies.** The ring buffer stores full snapshots by
    value. That is fine for depth 1–5, but the struct also carries legacy
    best-bid/ask fields alongside depth arrays. Consolidate to one
    representation.

16. **Heuristic timestamp normalisation.** `NormalizeEpochToMillis` infers units
    from magnitude. It is pragmatic, but any source with a non-standard epoch
    will be silently misclassified. Record the detected unit once per stream.

17. **Mixed naming and language.** German comments (`aggressiver verkauf`,
    `für`), `NEU` markers, and English elsewhere make the code harder to scan.
    Normalise comments and remove migration markers during the refactor.

18. **Header-only vs `.cpp` inconsistency.** `venue_spec` has both; many other
    modules are header-only. Pick a policy per layer (interfaces header-only,
    implementations in `.cpp`) and apply it consistently.

### Suspected dependency direction (target)

    core types (trade_event, timestamp_utils, trade_types)
        ↑
    aggregator (bars, footprint, L2 storage)
        ↑
    market_context → trigger / filter
        ↑
    trade_management / prop_guard
        ↑
    ibroker / streamer / venue
        ↑
    orchestrator / engine_state
        ↑
    UI / redis / logging

Current violations to remove:

- `ibroker.hpp` depending on `aggregator.hpp` for `SignalDirection`.
- Strategy headers depending on concrete storage types (`L2RingBuffer`,
  `SessionMetrics`) rather than a read-only market view.
- Execution and UI reaching into engine state directly.

---

## 5. Suggested Refactoring Order

1. **Extract the funnel.** Move `PrepareSignalRisk`, `FinalizeTakeProfit` and the
   sizer/risk/TP sequence into a `SignalExecutor` used by live, scan, WFA-IS,
   WFA-OOS and both replay paths. Delete the `execute_signal` copies. This is the
   highest-value change because it is the only one that protects money: the live
   copy is currently the least tested and the only one that can be wrong for
   real.
2. **Give every path the same account model.** Construct a `PropGuard` in the
   backtest/WFA/replay drivers, replace the hardcoded `10000.0` with
   `PaperTrader::kInitialBalance`, and surface guard state in `TestResult` so a
   WFA winner can be rejected for breaching limits.
3. **Fix the aggregation contract.** Add `Aggregator::Reset()` (or document
   single use), document the touched-span sentinel invariant in
   `ResetWorkspace`, and add a per-bar "already fired" latch in the funnel.
4. **Unify configuration.** One typed config struct with one tolerant reader
   (bool/number/string spellings) consumed by `AppConfig`, the factory and
   `ApplyRMultiplesManagement`. Delete the duplicated reader families. (Note:
   `RunConfig` already exists in `config_parser.hpp` for grid permutations —
   pick a different name for the unified type.)
5. **Extract core types.** `SignalDirection`, `TradeSignal`, `TradeRecord` and
   `StopEvent` into a dependency-free header, so `ibroker.hpp` and `filter.hpp`
   stop including `aggregator.hpp`. Adopt `Aggressor` in `TradeEvent`.
6. **One replay implementation.** Fold `ReplayWFA` into `ReplayManager`, and
   either expose HTF history or delete the second aggregator.
7. **One metrics definition.** Trades, win rate, drawdown and R-multiples
   computed once and consumed by CSV, TUI, WFA aggregation and the prop guard.
8. **Then optimise.** Per-bar indicator caching, footprint packing, the sliding
   grid `memmove`, and the per-bar `last_l2` copy (history holds one full
   snapshot per bar).

---

## 6. Scope & Confirmed Facts

**Reviewed:** `src/main.cpp`, `src/factory.cpp`, `src/live_engine.cpp`,
`src/mokuren.cpp`, `src/aggregator.cpp`, `src/bybit_broker.cpp`,
`src/hyperliquid_broker.cpp`, `src/bybit_streamer.cpp`,
`src/hyperliquid_streamer.cpp`, `src/binance_streamer.cpp`,
`include/trigger.hpp`, `include/market_data/format.hpp`,
`include/aggregator.hpp`, `include/market_context.hpp`, `include/ibroker.hpp`,
`include/engine_state.hpp`, `include/config_parser.hpp`,
`include/prop_guard.hpp`, `include/trade_management.hpp`,
`include/live_streamer.hpp`, `test/test_filters.cpp`.

**Reviewed (second pass):** `include/position_manager.hpp`,
`include/replay_manager.hpp`, `src/logger.cpp`, `include/ui_pagelayout.hpp`.
Nothing in this document is now inferred from call sites alone.

Confirmed facts used throughout this document:

- `MarketContext` membership: `history`, `live_bar`, `htf_bar`, `latest_l2` and
  `l2_history` are const references; `session` is a `SessionMetrics` by value
  (assignable in tests).
- `IBroker` method set: `ProcessSignal`, `CheckRisk`, `HasOpenPosition`,
  `ClosePartial`, `GetBalance`, `GetCurrentPosition`, `UpdateStopLoss`,
  `GetNetProfit`, `GetTradeHistory`, `GetLastTradeTime`, `GetTradeCount`,
  `GetLastClosedSize`.
- `kMaxQueueDepth` is 65536 (`include/live_engine.hpp`), `MAX_GRID_LEVELS` is
  5000 and `L2_RING_BUFFER_SIZE` is 1024 (`include/aggregator.hpp`).
  `MAX_L2_DEPTH` is referenced from `include/trigger.hpp` but defined alongside
  the `L2Snapshot` arrays in `include/trade_event.hpp`; `kMaxBookDepth` is 20
  (`include/market_data/format.hpp`).
- `EngineState` is a plain struct (`include/engine_state.hpp`), not a
  singleton: ownership is by the caller (UIManager / workers). The output
  decoupling in P1b item 9 is therefore a matter of introducing a result sink,
  not of removing a global.
- `IMarketStreamer` callbacks (`Trade`, `L2`, `Finished`, `Error`) are invoked
  from the streamer's own thread; `Finished` and `Error` fire at most once and
  never concurrently. `Stop()` never fires `Finished`.
- `BybitBroker::CheckRisk` is intentionally empty; the hard stop and target are
  attached to the entry order and enforced by the venue. `GetBalance()` is
  `totalEquity` from the private `wallet` push, so it is `0.0` until that frame
  arrives.
- `HyperliquidBroker` cannot trade: `SignOrderEIP712` returns an empty
  signature and every order path only logs.
- `PositionManager` measures 1R from the placed stop (`|entry - stop|`), not
  from `risk.sl_pct`, and confirms each stop move by re-reading
  `GetCurrentPosition().stop_loss`. A rejected move is retried at most once per
  second (`kStopRetryCooldownMs`). `SetEnabled(false)` is how `PaperTrader`
  avoids running the ladder twice.
- `ReplayManager::Run` is the sixth funnel copy. It validates the stop before
  sizing but not after the risk firewall, and it re-derives the take profit
  from `cfg.tp_pct / cfg.sl_pct` instead of calling `FinalizeTakeProfit`.
- `ShutdownLogger()` resets the `std::once_flag`, so `InitLogger()` can be
  called again after a shutdown.

## 7. Test surface today

`test/test_filters.cpp` builds a `MarketContext` by hand and exercises eight
filters (MinVolume, POCTrend, OrderbookImbalance, VwapTrend, MacroTrend,
CVDDivergence, Volatility, RVOL). That pattern is right and should be extended
to the pieces above:

- **Aggregator**: gap larger than the window, an interval with no prints,
  session rollover, reuse across two datasets.
- **Funnel**: a risk module that moves the stop to the wrong side, TP derivation
  when the strategy leaves it at 0, stop on the wrong side before sizing.
- **Prop guard**: paper floating P&L, breach on a quiet book with no trade
  prints, flatten-once-on-transition.
- **Config parity**: the same JSON through `AppConfig::Load`,
  `StrategyFactory::Build` and `ApplyRMultiplesManagement`, asserting the ladder
  numbers match.

One caveat on the current suite: the `VolatilityFilter` case asserts a result
for a bar with `high < low` (`mock_history.back().high = 59900.0` against a
`low` of `60000.0`). That only holds if the filter averages `high - low`
without `abs`, so the test encodes a non-obvious behaviour for impossible data
rather than guarding against it.
