# Known bugs and rough edges

Append-only. One entry per issue; keep it short.

## AND_Trigger silently drops tick triggers
- `include/trigger.hpp`: `AND_Trigger` overrides `EvaluateCandle` only, so it
  inherits `ITrigger::EvaluateTick`, which returns `NONE`.
- A tick-only trigger inside an AND can never fire, and the config gives no
  indication.
- Fix: override `EvaluateTick` with the same consensus logic `OR_Trigger` uses.

## Filters fail open when book or POC data is missing
- `include/filter.hpp`: `OrderbookImbalanceFilter` returns `true` when the
  opposite side's quantity is `<= 0`; `POCTrendFilter` returns `true` when
  `poc_price == 0`.
- Every other filter fails closed on missing data, so a stale or empty book
  passes the one filter meant to require it.
- Fix: apply one policy (fail closed) to both.

## TimeOfDayFilter depends on host timezone
- `include/filter.hpp` uses `localtime_r` on the bar timestamp.
- Backtests are not reproducible across machines, and a live run can disagree
  with a UTC replay.
- Fix: use `gmtime_r`, or add an explicit timezone to the config.

## VwapTrendFilter reads a different price than its siblings
- `include/filter.hpp` uses `context.live_bar.close`; every other filter uses
  the last history bar.
- When the live bar is empty (`close == 0`), a BUY is always rejected
  (`0 < vwap`) and a SELL always passes. The failure is asymmetric and silent.
- Fix: use `history.back().close`, or guarantee live-bar population on every
  path.

## SpoofHunterTrigger assumes a full five-level book
- `include/trigger.hpp` sums `min(depth_levels_, MAX_L2_DEPTH)` entries from
  fixed arrays. `L2Snapshot` carries no per-side level count, so unused slots
  read as `0.0`.
- A five-level lookback compared against a one-level snapshot looks like a
  100% wall removal and emits a signal.
- Fix: carry level counts (or a zero-price sentinel) and aggregate only real
  levels.

## MaxLeverageLock passes on negative equity
- `include/risk_engine.hpp`: `current_leverage = notional / GetBalance()`.
- With `balance < 0` the ratio is negative, so `> max_leverage_` is false and
  the lock allows the trade.
- Fix: guard `balance <= 0` and reject explicitly.

## Replay failure is indistinguishable from an empty replay
- `src/mokuren.cpp`: `ReplaySingleRun` catches `std::exception`, logs it and
  returns a default-constructed `ReplayResult`; callers see zero trades.
- It also falls back to the dataset `"data"` when `environment.dataset` is
  absent, so a config typo reads a directory the user never named.
- Fix: carry an error in the result (or rethrow), and require an explicit
  dataset outside the TUI.

## MaxDrawdownLock is unreachable from config
- `src/factory.cpp` handles SinglePositionLock, MaxStopDistanceLock,
  MaxLeverageLock and AntiRevengeLock only; `MaxDrawdownLock` is defined in
  `include/risk_engine.hpp` but no config name reaches it.
- `max_daily_loss` is passed to `PaperTrader` only. A `--live` run has no
  account-level loss cap unless another module happens to cover it.
- Fix: add the factory case (wired to the venue broker), or delete the class.

## Trigger composition is OR-only
- `src/factory.cpp` always builds `OR_Trigger`; `AND_Trigger` is never
  constructed from a config.
- There is no way to express "stacked imbalance and delta agreement".
- Fix: honour an `operator` key (`or` default, `and`).

## Grid scanner and factory disagree on disable spellings
- `GridScanner::FindParams` skips on bool false, `"false"`, `"0"` and integer 0.
- `IsDisabled` in `src/factory.cpp` also accepts `"False"`, `"FALSE"` and
  float `0.0`.
- A component with `"enabled": "False"` is swept by the grid scanner and
  skipped at runtime: the sweep reports results for a strategy that never ran.
- Fix: one shared predicate.

## A grid node without `val` silently becomes a default
- `FlattenGrids` collapses a node only when `val` is a number or numeric
  string; otherwise the object survives.
- `SafeNumber`/`ParamNumber` then see an object and return their fallback, so
  the live run uses a default the author never wrote.
- Fix: reject a grid node without a scalar `val` at load time.

## `scale_out` levels are required even when the block is disabled
- `AppConfig::Load` requires `trigger_r`/`trigger_pct` and `fraction` for
  `scale_out` regardless of `enabled`; `break_even` has the same requirement but
  its flag is optional.
- Intended for consistency (a config means the same thing on and off), but a
  feature that is switched off still aborts on a stale value it will never use,
  and `Validate` gates only the trailing *ordering* check on the flag.
- Fix: document the requirement prominently, or gate the level checks on
  `enable_scale_out` as trailing's ordering check already is.

## `ParamInt` truncates silently
- `src/factory.cpp`: `ParamInt` casts through `double`, so `"lookback": 5.9`
  becomes 5 and no diagnostic is emitted.
- Affects every integer component parameter (`levels`, `lookback`,
  `lookback_ms`, `cooldown_ms`, time-of-day fields).
- Fix: reject non-integral values, or round explicitly with a warning.

## Venue-path locks reject everything until private state arrives
- `src/factory.cpp` binds `MaxLeverageLock` and the sizer to the venue broker,
  whose cached balance is 0 until the first private websocket frame.
- `MaxLeverageLock` divides by that 0 and rejects, and the sizer yields no
  volume, so the first signals after arming are silently dropped.
- Fix: expose a "state ready" flag and treat it as unarmed rather than as a
  failed check (and avoid the divide-by-zero while there).

## Integer component params accept zero and negatives
- `src/factory.cpp` reads every integer param through `ParamInt` with no lower
  bound: `StackedImbalance.levels`, `RVOL`/`ATRChop`/`Volatility`/
  `CVDDivergence.lookback`, `SpoofHunter.lookback_ms`,
  `AntiRevengeLock.cooldown_ms`, the time-of-day fields.
- `lookback: 0` leaves RVOL/Volatility averaging zero elements (NaN, so the
  filter rejects forever) and ATR summing nothing; `levels: 0` fires on a candle
  with an empty footprint; `cooldown_ms: 0` silently disables AntiRevenge.
- Fix: validate per-parameter bounds once, in `AppConfig::Validate`.

## A grid node with non-numeric bounds is ignored twice
- `GridScanner::FindParams` treats an object as a grid node only when `min`,
  `max` and `step` are JSON numbers; `FlattenGrids`/`IsGridNode` additionally
  require a scalar `val`.
- `{"min": "0.001", "max": "0.004", "step": "0.001", "val": "0.002"}` is
  therefore neither swept nor collapsed: the backtest runs a single value and
  the live path hands an object to `SafeNumber`, which returns its fallback.
- Fix: accept numeric strings in both predicates, or raise a grid error on a
  non-numeric bound.

## `tp_r` failures are reported as `risk.tp_pct`
- `AppConfig::Load` resolves `tp_r * sl_pct` into `tp_pct`, then `Validate`
  range-checks the result and names `risk.tp_pct`.
- `{"sl_pct": 0.002, "tp_r": 60}` aborts with "risk.tp_pct must be in
  (0.0, 0.1)" although `tp_pct` appears nowhere in the file.
- Fix: range-check `tp_r` where it is read and name the key the author wrote.

## `tp_r` rejects the numeric strings the rest of the loader accepts
- `include/trade_management.hpp`: `ResolveTpPct` and `ResolveRiskPcts` gate the
  take-profit key on `is_number()`, so `"tp_r": "2.0"` aborts with
  "'risk.tp_r' (or legacy 'risk.tp_pct') is required" — naming neither the value
  nor the type.
- `sl_pct` is read through `RequireNumberAny`, which does accept numeric
  strings, so the two halves of the same `risk` block disagree about what a
  number is.
- Fix: one tolerant scalar reader shared by the whole `risk` section.

## A `venue` object silently discards a root-level `is_testnet`
- `src/config_parser.cpp`: `is_testnet` is consulted only in the `else if` of
  `if (j.contains("venue"))`. With `{"is_testnet": false, "venue": {"symbol":
  "BTCUSDT"}}` the venue branch runs, finds no `environment`, and leaves
  `cfg.env` at its default `Testnet`.
- The result is a run that says mainnet in the file and streams testnet data,
  with no diagnostic and nothing for `Validate()` to catch.
- Fix: fall back to `is_testnet` inside the venue branch when `environment` is
  absent, and warn when both are present and disagree.

## `prop.daily_reset_hour_utc` is ignored unless it is a JSON integer
- `src/config_parser.cpp` guards it with `is_number_integer()`; a string
  `"0"` or a float `0.0` is dropped silently and the guard default is kept.
- Every neighbouring numeric field accepts numeric strings, so a hand-written
  `"daily_reset_hour_utc": "0"` changes behaviour with no diagnostic, and
  `Validate()` cannot catch it because it never sees the value.
- Fix: read it through the same tolerant reader the rest of the section uses,
  then range-check.

## The shipped systemd unit hardcodes another project's paths
- `deploy/mokuren@.service` names `User=bunta`, `WorkingDirectory=/home/bunta/src/kaito`
  and `ExecStart=/home/bunta/src/kaito/build/mokuren`.
- A copy without edits fails at `200/CHDIR`, and `Restart=always` turns that into
  a five-second loop that fills the journal.
- Fix: use `%h`, install-time substitution, or document the required edits in a
  comment at the top of the file.

## The unit has no restart limit
- `Restart=always` + `RestartSec=5` with a config error, an unreachable venue or
  a gate that cannot pass at the current hour produces an unbounded loop.
- Nothing in the unit or the engine backs off or gives up.
- Fix: ship `StartLimitIntervalSec` / `StartLimitBurst` in `[Unit]`.

## The unit does not pin a timezone
- `TimeOfDayFilter` uses `localtime_r`, so the same config filters different
  hours on hosts with different `TZ`.
- The unit sets no `Environment=TZ=`, so a daemon inherited the host's setting.
- Fix: set `Environment=TZ=UTC` (and align the filter's clock with it).

## `RunBacktest` reports a failure twice
- `src/main.cpp`: `RunBacktest` catches, logs, prints to stderr and then
  **rethrows**; the outer handler in `main` logs and prints the same message
  again.
- A single fatal error produces two log lines and two stderr lines, which reads
  like two failures.
- Fix: let one layer report it.

## The Telegram notifier interpolates unescaped values into Markdown
- `tools/TELEGRAM.py`: every message is sent with `parse_mode: "Markdown"`, and
  `reason`, `exit_reason` and `symbol` are interpolated verbatim.
- A value containing `_ * ` [` (a venue error string, a halt reason) makes
  Telegram answer 400. `send_telegram` logs it to stderr and returns `False`;
  the event is gone.
- Fix: escape `_*`[]()~>#+-=|{}.!` in every dynamic value, or drop to plain text
  for messages that carry free-form strings.

## The Telegram notifier has no retry or queue
- `tools/TELEGRAM.py`: one `requests.post` per event, 10 s timeout, no backoff
  and no persistence.
- A 429 rate limit, a 5xx or a transient network error loses the notification
  permanently; restarting the process does not replay it.
- Fix: a bounded in-memory queue with retry on 429/5xx (honouring
  `retry_after`), or spool to disk so a restart can resume.

## Dataset caches are never invalidated by a changed CSV
- `src/data_manager.cpp`: `trades_cache_v2.bin` is read whenever it exists;
  `L2_cache_v3.bin` is converted only `if (!fs::exists(bin_file) && ...)`.
- Neither path compares against the CSV's mtime or size. Replacing `trades.csv`
  and re-running replays the previous dataset with no diagnostic.
- Fix: store source size+mtime in the cache header, or drop the cache when the
  CSV is newer.

## An unrecognised trade CSV header yields zero trades, silently
- `src/data_manager.cpp`: the layout check requires `ts`, `price`, `qty` and
  (`maker` or `side`). Failing it leaves `all_trades` empty, no cache is
  written and the function returns success.
- The caller cannot distinguish "no trades in the file" from "the file was not
  understood"; a replay then reports no setups.
- Fix: return a status, or log at error level with the first line of the file.

## Rows before 2017-07 are dropped without a warning
- `src/data_manager.cpp`: both readers gate on `timestamp > 1500000000000`
  (2017-07-14) to filter header junk and unit-mangled values.
- A legitimate older dataset loads empty, and the only clue is a zero count.
- Fix: log the number of dropped rows once per file, with the first timestamp.

## A refused L2 conversion leaves the run book-blind and successful
- `src/data_manager.cpp`: `ConvertL2CsvToBin` returns false when the recognised
  layout maps to zero levels; `LoadAllL2Snapshots` then returns an empty vector
  with no error.
- `ValidateDataAlignment` returns `true` when either channel is empty, so the
  replay proceeds with no book at all and every L2-dependent filter and trigger
  degrades to its missing-data branch.
- Fix: make the loader throw (as it does for a damaged `.mkn`), and make
  `ValidateDataAlignment` fail on an empty channel when the other one is not.

## Volume is rounded to a hardcoded 0.001 lot step and capped at 20x
- `src/factory.cpp` constructs `PositionSizer(active_broker, risk, min_dist)`
  only, so the class defaults `lot_step = 0.001` and `max_leverage = 20.0`
  apply to every instrument and cannot be configured.
- A venue with a coarser step (1, 0.01) receives volumes it rejects; a finer
  step is under-sized. A volume that rounds to 0 drops the signal with a
  warn-level line, which is invisible at the default log level.
- `MaxLeverageLock` applies a second, independent leverage check with its own
  `max_leverage`, so the two limits can disagree.
- Fix: read `lot_step` and the cap from the instrument or the config, and let
  one of the two define the ceiling.

## Trade-management R levels reject numeric strings
- `include/trade_management.hpp`: `ResolveLevelPct` and `ResolveTpPct` gate on
  `is_number()`, and `ResolveRiskPcts` accepts strings only for `sl_pct`.
- `{"break_even": {"trigger_r": "1.0"}}` aborts with "requires 'trigger_r' (or
  legacy 'trigger_pct'); nothing is defaulted", naming neither the value nor the
  type, while the same file's `sl_pct` may be a string.
- Fix: one tolerant scalar reader for the whole `risk` /
  `trade_management` surface.

## `replay_manager.hpp` describes a precedence that no longer exists
- The comment above `ResolveRiskPcts` in `include/replay_manager.hpp` says the
  legacy `sl_price_pct` is checked first and therefore disagrees with
  `AppConfig::Load`.
- `include/trade_management.hpp` now reads `sl_pct` first, so both agree and the
  paragraph documents a divergence that was removed.
- Fix: delete the paragraph.

## The TUI dataset selector cannot see a recording
- `include/ui_menu.hpp`: the `dataset` row is filled by `ScanDirectory("data",
  true)`, which returns sub-directories only.
- A `.mkn` recording — the format the replay is designed around — can never be
  selected in the config matrix; only legacy CSV directories can.
- Fix: list `.mkn` files alongside directories, or add a dedicated selector.

## Saving a grid range in the TUI overwrites its `val`
- `include/ui_menu.hpp`: the `SUBMENU_NAV` save path builds
  `{mode, min, max, step, val}` with `val = min`.
- A profile where `val` sits inside the range (or above `min`) silently starts
  using the lower bound the next time the grid editor is opened and saved —
  including for a later `--live` run of that profile.
- Fix: preserve the existing `val`, or make it a fourth editable field.

## `environment.max_cores` ignores numeric strings
- `src/config_parser.cpp` reads `max_cores` only when the value
  `is_number_integer()` and > 0.
- `"max_cores": "8"` falls back to `hardware_concurrency()` with no diagnostic,
  unlike every neighbouring numeric field.
- Fix: use the shared tolerant reader.

## The equity page computes drawdown against a hardcoded 10,000
- `include/ui_equity.hpp`: `CalculateCurve()` starts `current_equity` and
  `peak_equity` at `10000.0` and derives `max_dd_pct` from them.
- `PaperTrader` seeds `initial_balance_` from the config precisely so drawdown
  is measured against funded capital; the page ignores that, so any other
  starting balance shows a wrong max drawdown and a mis-scaled curve.
- Fix: take the starting equity from the replay's config.

## A running scan writes into a destroyed UIManager
- `include/ui_scanner.hpp`: the sweep runs in a detached `std::thread` that
  captures `EngineState*` — a `UIManager` member — and writes to it on every
  result and progress callback.
- `UIManager::~UIManager` only calls `notcurses_stop()`, and `Q` leaves the loop
  without waiting, so quitting mid-scan leaves the thread writing freed memory.
- Fix: keep the thread joinable and join it in the destructor (or request a stop
  and join).

## The TUI's generated default profile cannot be loaded
- `include/ui_menu.hpp`: `CreateDefaultConfig()` writes `backtest`,
  `environment`, `execution`, `filters`, `risk`, `trade_management` and
  `triggers`, but no `exchange` and no `venue`.
- `AppConfig::Load` treats a missing `exchange` as a hard error, so on a fresh
  checkout the first scan aborts on a file the editor itself just wrote.
- Fix: put an `exchange` (and a `venue` block) in the template, or relax the
  requirement for a backtest-only load.

## A prop-breached run is ranked like any other
- `include/test_result.hpp` carries `prop_breached` "so a permutation that would
  have ended a funded account is visible in the results table".
- `include/ui_scanner.hpp`'s `result_cb` copies profit, win rate, drawdown,
  Sharpe, Sortino, MC drawdown and trade count into `UIResult` but not
  `prop_breached`, and `UIResult` has no field for it: a breached run can sit at
  rank 1 with nothing marking it.
- Fix: carry the flag into `UIResult` and mark the row.

## TRADELOG's ENTER destroys the page its own handler runs in
- `include/ui_manager.hpp`: the trade log's `on_inspect_trade` callback calls
  `SwitchPage(PageID::INSPECTOR)`, which replaces `current_page` and therefore
  destroys the `UITradelog` whose `HandleInput` is on the stack.
- It returns immediately afterwards, so this is benign today; any member access
  after the callback would be use-after-free. `JumpToCandle` is also commented
  out, so ENTER only switches pages.
- Fix: defer the switch until `HandleInput` returns, and wire the jump.

## Grid search, WFA and replay order a same-millisecond book frame differently
- `src/grid_runner.cpp` and `src/wfa_runner.cpp` (both the IS and OOS loops)
  catch book frames with `timestamp <= trade.timestamp`, so a frame sharing the
  print's millisecond is applied **before** the trade; the replay path applies
  it **after**, matching the live stream's "print before book" ordering.
- The same dataset therefore produces different fill prices, different tick
  triggers and different results depending on which path runs it, and a grid
  optimum that depends on book timing does not reproduce in a replay.
- Fix: use one comparator, defined once next to `MarketContext`.

## A `grid` axis makes most permutations vanish silently
- `src/grid_runner.cpp` validates `runs.front().full_json` once on the driver
  thread and then loads every permutation inside a worker.
- A sweep whose later values fall outside `Validate()` (e.g.
  `sl_pct` up to 0.2, `lookback` down to 0) throws per worker, is logged at
  error level and never reaches `on_result`. The scan reports
  `SCAN COMPLETE` with a fraction of the permutations evaluated.
- Fix: validate every generated config before the workers start, and fail the
  scan with the first offending parameter value.

## A run that throws never advances the progress counter
- `src/grid_runner.cpp`: `++current_iteration` sits after the result block,
  inside the per-run `try`.
- Any throwing permutation leaves the bar permanently short of `total`, so a
  scan that skipped half its runs looks like one that is still working.
- Fix: move the increment into a scope guard that runs on both paths.

## `step: 0` is silently coerced to `1.0`
- `src/config_parser.cpp`: `const double step = (p.step == 0.0) ? 1.0 : p.step;`
  before the range check, so a zero step never reaches the validation that
  rejects a negative one.
- A config meaning "don't sweep this" but written as `"step": 0` produces a
  unit-step sweep whose bounds were probably chosen for a different step, and
  the report gives no hint that the value was replaced.
- Fix: reject `step <= 0` and let `min == max` express "one value".

## WFA drawdown ignores the configured starting balance
- `src/wfa_runner.cpp`: the aggregate metrics start `current_balance` and
  `peak_balance` at `PaperTrader::kInitialBalance`.
- `--paper` seeds from `initial_balance` and the prop guard from
  `prop.initial_balance` precisely so drawdown has a defined denominator; the
  WFA report uses neither, so the same trades on a 5,000 account report a
  different max drawdown than the replay of that config.
- Fix: take the base from the winning config, as `PaperTrader` does.

## A WFA window with all-throwing permutations still runs OOS
- `src/wfa_runner.cpp`: `best_config` is initialised to `runs[0]` and only
  reassigned inside the per-run `try`, after the strategy succeeded.
- If every permutation throws (a bad grid bound past `Validate()`, a missing
  instrument), the OOS pass runs `runs[0]` — an arbitrary config — and the
  window reports a normal result. The only trace is the per-worker log line.
- Fix: leave `best_config` unset, skip the OOS pass, and report the window as
  failed.

## The aggregate WFA result's `full_config` is one window
- `src/wfa_runner.cpp`: `final_cfg = collected_configs.back().config` with
  `backtest.wfa_mode = true` is stored as `res.full_config`.
- Anything that replays `full_config` (a leaderboard row, a CSV reader) replays
  the last window alone while the row's metrics describe the stitched
  out-of-sample run. The TUI uses `wfa_configs` and is unaffected, so the
  mismatch only shows up outside it.
- Fix: store the base config, or omit `full_config` for an aggregate result.

## WFA reads each window twice from disk
- `src/wfa_runner.cpp`: `all_trades` / `all_l2` are loaded once, then
  `SliceTrades` / `SliceL2` copy per window into fresh vectors for every
  in-sample worker to share by reference.
- Each window's in-sample slice is a full copy of the window's prints, so peak
  memory is the whole dataset plus one window per concurrent read, and the copies
  are made serially in the window loop.
- Fix: slice once per window into a shared immutable view (span), and let workers
  read it.

## `WfaRunner::ReplayWFA` dynamic_casts the broker with no check
- `src/wfa_runner.cpp`: `auto* oos_pt = dynamic_cast<PaperTrader*>(oos_eng.ptrader.get());`
  then `oos_pt->GetTradeHistory()` is dereferenced unconditionally.
- Safe only because the factory always fills `ptrader`; a future broker swap
  turns this into a null dereference in a path that cannot report it.
- Fix: check the cast, or have the factory expose the trade history through
  `IBroker`.

## The `prop` block accepts trailing garbage in numeric strings
- `src/config_parser.cpp`: the section-local `read_num` calls `std::stod` and
  returns on the first parseable prefix, unlike `SafeNumber`, which requires the
  whole string to be consumed.
- `"max_daily_drawdown_pct": "0.04x"` is silently read as 0.04, and
  `"0.04x"`-style typos elsewhere are rejected — the same file disagrees with
  itself about what a number is.
- Fix: reuse `SafeNumber` (or the shared tolerant reader) and range-check.

## Gate rate is cumulative, not windowed
- `src/startup_gate.cpp`: `required_count` scales the per-minute rate by
  `elapsed` since `start_ms_`, and `delivered` is a running total.
- The check is named "sustained", but a feed that delivers everything in the
  first second and then goes silent passes a soak it never sustained; a feed that
  starts late fails even while delivering a healthy rate at the end.
- Fix: evaluate the rate over a trailing window (or reset counters per window)
  so "sustained" means what it says.

## The prop guard is seeded from a different balance per path
- `src/factory.cpp` hands `cfg.initial_balance` to `PaperTrader` only when
  `live_mode && use_paper_broker` (`--paper`). Every other path gets
  `PaperTrader::kInitialBalance`.
- `src/grid_runner.cpp` and `src/wfa_runner.cpp` then fall back to
  `PaperTrader::kInitialBalance` for `prop.initial_balance`, never to the root
  `initial_balance`.
- A 5,000 funded config is therefore guarded against 10,000 in every backtest
  and against the real balance on the venue: the backtest neither warns early
  nor halts where the live account would.
- Fix: seed from the config's `initial_balance` on every path, or require
  `prop.initial_balance` when `prop.enabled` is true.

## A book-gap sentinel is processed before it is detected
- `src/live_engine.cpp`, `WorkerLoop`: the L2 branch calls
  `aggregator->ProcessL2(ev.l2)` and only then checks
  `best_bid_price <= 0.0 || best_ask_price <= 0.0`.
- The zeroed frame has already entered the book window, `GetLatestL2()` and the
  session metrics by the time the gap latch is set, so the next
  `MarketContext` carries a book with a zero bid/ask.
- Fix: test the sentinel first and return before touching the aggregators.

## SYSTEM_STARTUP / SYSTEM_SHUTDOWN say almost nothing, and start too early
- `src/live_engine.cpp`: both events carry `action` and a fixed `msg`. The
  notifier (`tools/TELEGRAM.py`) reads `dataset`/`mode` for startup and `reason`
  for shutdown, so every one of those fields renders `n/a`.
- Startup is also published before `router_->Poll()` runs the checks, so a run
  that fails its gate still announces "Engine online" and then exits.
- Both live modes publish identically, so a paper session cannot be told from a
  funded one in the chat.
- Fix: carry `venue`, `symbol`, `mode` and a real `timestamp`, and either move
  the startup event after the gate or add a distinct `SYSTEM_GATE_FAILED`.

## A throwing scan takes the whole TUI down
- `include/ui_scanner.hpp`: the sweep runs in a detached `std::thread` whose
  body calls `RunGridSearch` / `RunWFA` with no `try`.
- `src/grid_runner.cpp` calls `json::parse` and `GridScanner::GenerateGrid`
  unguarded, so a malformed profile or a `grid error` range escapes as an
  exception; one leaving a detached thread calls `std::terminate`.
- `src/wfa_runner.cpp` has the same hole around `AppConfig::Load` and
  `StrategyFactory::Build` for the out-of-sample pass.
- Fix: catch in the thread body, report on the status line, clear `is_running`.

## A winning run without a dataset path kills the TUI
- `include/ui_manager.hpp`: the scanner's `on_inspect` callback throws when
  `find_path` finds no `filepath` / `data_path` / `dataset`.
- It is invoked from `UIScanner::HandleInput` on ENTER, inside the UI loop, and
  nothing catches it: the exception unwinds out of `UIManager::Run`. The
  `SwitchPage(SCANNER)` guard throws the same way.
- Fix: set a status line and return.

## The file pickers offer a directory as the first option
- `include/ui_menu.hpp`: `ScanDirectory` pushes `folder` itself before
  iterating, so `data` appears in the dataset list and `configs` in the profile
  list as selectable values.
- Selecting `configs` sets `config_path = "configs"`; `LoadConfigSmart` cannot
  open a directory, silently substitutes the built-in default, and every later
  save writes to a path that never opens.
- Fix: skip the seed entry, or mark it non-selectable.

## The details overlay says any key closes it, but three keys do
- `include/ui_scanner.hpp`: the overlay prints `[ANY KEY] CLOSE OVERLAY`; the
  handler closes only on ESC, ENTER, `q`/`Q`, and lets UP/DOWN fall through to
  move the leaderboard selection underneath.
- Fix: close on any key, or print the keys that work.

## A walk-forward leaderboard row replays one window
- `include/ui_scanner.hpp` decides between the two replay paths with
  `params_str.find("WFA_Mode") != std::string::npos`.
- `src/wfa_runner.cpp` writes `res.parameters["WFA_Windows_Done"]`, so the
  substring never matches: ENTER always takes the
  `ReplaySingleRun(…, full_config)` branch, and `ReplayWFA` is unreachable.
- The row's metrics describe the stitched out-of-sample run while the trade log
  shows only the last window's config, so the two disagree by construction.
- Fix: carry an explicit `is_wfa` flag on `UIResult` (or match
  `wfa_configs.empty()` instead of a substring).

## `backtest.wfa_mode` is never read
- `src/wfa_runner.cpp` reads `wfa_in_sample_days`, `wfa_out_of_sample_days` and
  `wfa_step_days` only; `wfa_mode` appears nowhere in the runner.
- The workflow is chosen by the `S` / `W` key in `include/ui_manager.hpp`, and
  `wfa_mode` is written into an aggregate result's config after the fact — so a
  scheduled or scripted run cannot select walk-forward from the file.
- Fix: read the key where the two workflows are dispatched, or drop it from the
  profile template.

## `ArmAutostart` records state that nothing uses
- `include/ui_scanner.hpp`: `pending_autostart_` / `pending_autostart_key_` are
  set by `ArmAutostart()`, and `HandleInput`'s only consumer is
  `if (pending_autostart_) { if (key != pending_autostart_key_) { … } else { … } }`
  where both branches do the same thing: clear the flag.
- A duplicated `S` in the same frame therefore starts a second scan once the
  first has finished, which is exactly what the flag was added to prevent.
- Fix: delete the state, or make the guard actually reject a second start.

## A profile with a top-level `error` key is treated as corrupt
- `include/ui_menu.hpp`, `LoadConfigSmart`: after a successful `json::parse` it
  throws when `config_data.contains("error")`, which lands in the same `catch`
  that substitutes the built-in default and rewrites the file.
- Any config that happens to carry an `"error"` key — a notes field, a copied
  API response — is silently replaced on open.
- Fix: drop the check, or test the parse result rather than a payload key.

## A `.csv` in `configs/` is offered as a profile and then overwritten
- `include/ui_menu.hpp`: the `config_profile` selector uses
  `ScanDirectory("configs", false)`, which accepts `.csv` as well as `.json`.
- Selecting one sets `config_path` to the CSV; `LoadConfigSmart` fails to parse
  it, substitutes `CreateDefaultConfig()` and writes it back over the CSV. The
  dataset is destroyed by an ordinary navigation.
- Fix: list `.json` only for the profile row.

## The heatmap's own close key is unreachable
- `include/ui_scanner.hpp`: the heatmap handler closes on `q`/`Q`, alongside
  `ESC` and `H`.
- `UIManager::Run` consumes `q`/`Q` before dispatching to the page, so that
  branch is dead: pressing it quits the whole TUI instead of closing the
  overlay. The details popup has the same dead `q`/`Q` branch.
- Fix: drop `q`/`Q` from both handlers, or have `UIManager` honour
  `BlocksGlobalHotkeys()` for open overlays.
