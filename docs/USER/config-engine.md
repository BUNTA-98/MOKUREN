# Engine settings

The blocks that describe the run rather than the strategy. Component lists are
in [Components](config-components.md).

## `exchange`

Required. A string (`"bybit"`, `"hyperliquid"`) or an object carrying the name
plus credentials:

```json
"exchange": {"name": "bybit", "api_key": "…", "api_secret": "…"}
```

Only `bybit` has both a streamer and an execution adapter. `hyperliquid` has a
streamer, so it works with `--paper` and `--probe`; `--live` aborts with
`no execution adapter for exchange 'hyperliquid'`.

## Credentials and environment

| key | default | notes |
| --- | ------- | ----- |
| `api_key` | `""` | required for `--live` on bybit |
| `api_secret` | `""` | required for `--live` on bybit |
| `is_testnet` | `true` | legacy; ignored when `venue.environment` is set |

`--paper` and `--probe` never sign a request, so they load without credentials.

Ordering of the environment decision:

1. `venue.environment` — `mainnet`, `testnet`, `demo`.
2. `is_testnet` — `true`/`false`, mapping to testnet/mainnet. Read only when
   there is no `venue` object at all.
3. Default: testnet.

The second step is the sharp edge: a `venue` block *without* an `environment`
key leaves the default (testnet) in place, and a legacy `"is_testnet": false`
next to that block is ignored. Put `environment` in the `venue` block, or omit
the block entirely.

Mainnet on bybit requires `allow_mainnet: true` in `venue` or at the root. This
is deliberately a second key: a typo alone cannot route real orders.

## `venue`

| key | default | meaning |
| --- | ------- | ------- |
| `environment` | `testnet` | alias `env`; `mainnet`, `testnet`, `demo` |
| `category` | `linear` | `linear`, `spot`, `inverse` |
| `symbol` | `BTCUSDT` | venue symbol |
| `position_mode` | `one_way` | `one_way` or `hedge` |
| `allow_mainnet` | `false` | required for mainnet |

`position_mode: "hedge"` is accepted but not implemented: the adapter logs a
warning and sends one-way orders.

The venue name, environment, category and symbol are resolved once and handed
to both the streamer and the broker, so they cannot disagree. For hyperliquid
the symbol is the bare coin (`BTC`), not a pair; a symbol containing `USDT` or
`USDC` logs a warning because the venue will reject the subscription.

## `environment`

| key | required | default | meaning |
| --- | -------- | ------- | ------- |
| `interval_ms` | yes | — | candle interval |
| `macro_interval_ms` | yes | — | higher-timeframe interval, must be ≥ `interval_ms` |
| `tick_size` | yes | — | price tick, used by footprint and by the stops some triggers derive |
| `vwap_reset_hour` | yes | — | UTC hour the session VWAP resets (0–23) |
| `filepath` | no | `""` | legacy field |
| `dataset` | no | — | the dataset the scan and the replay read. The scanner uses the last value under `filepath`, `data_path` or `dataset` anywhere in the profile; a runner called without a path falls back to this key and then to `data` |
| `max_cores` | no | hardware concurrency (min 4) | worker count |

`tick_size` must match the instrument. A zero tick makes `DeltaAbsorption`
place its stop on the candle edge instead, and forces `StackedImbalance` to
infer a tick from the footprint.

## `execution`

Required (falls back to `environment` for older configs).

| key | default | note |
| --- | ------- | ---- |
| `taker_fee_pct` | required | charged on entry and exit |
| `slippage_pct` | required | applied to fills |

Both are fractions. They change every pnl figure in a report, which is why
neither has a fallback.

## `risk` (account-level keys)

| key | required | default | meaning |
| --- | -------- | ------- | ------- |
| `sl_pct` | yes | — | 1R; also the denominator for every `*_r` level. Alias: `sl_price_pct` |
| `tp_r` | one of | — | take-profit as a multiple of 1R |
| `tp_pct` | one of | — | take-profit as an absolute fraction (legacy) |
| `risk_per_trade_pct` | yes | — | equity fraction risked per trade |
| `min_distance_dollars` | no | `0.0` | minimum stop distance in quote currency; `0` never widens the trigger's stop |
| `max_daily_loss` | yes | — | daily loss limit handed to the paper account |
| `modules` | no | empty | risk firewall, see [Components](config-components.md) |

Take-profit resolution: `tp_r` wins over `tp_pct`. A config naming neither
fails to load instead of inheriting a guess. Because `sl_pct` defines 1R, the
same `tp_r` means a different absolute distance on a different `sl_pct`.

`max_daily_loss` is passed to the paper trader. A live run trades through the
venue adapter, which is not constructed with it — on the venue path, configure
`SinglePositionLock` and the other modules explicitly.

`min_distance_dollars` is absolute, not a percentage: `10` is noise for BTC and
a large stop for SOL. Raise it per instrument only when a trigger's structural
stop is tighter than the venue allows.

The sizer holds two limits the config cannot reach. It caps notional at 20×
balance and rounds volume down to a 0.001 step; a signal whose rounded volume is
0 is dropped with a warning in the log. `MaxLeverageLock` is a separate check
with its own `max_leverage`, and the two are not derived from each other.

## `trade_management`

Required. All three blocks are required; levels are required within them.

Levels are written in R (`trigger_r`, `target_r`, `distance_r`). The legacy
absolute keys (`trigger_pct`, `target_pct`, `distance_pct`) are honoured only
when the R key is absent. A missing level is a load error.

```json
"trade_management": {
  "break_even": {"trigger_r": 1.0, "target_r": 0.2},
  "trailing":   {"enabled": true, "trigger_r": 1.5, "distance_r": 0.5},
  "scale_out":  {"enabled": false, "trigger_r": 1.0, "fraction": 0.5}
}
```

| block | key | required | meaning |
| ----- | --- | -------- | ------- |
| `break_even` | `trigger_r` | yes | profit, in R, at which the stop moves |
| | `target_r` | yes | where the stop moves to, in R |
| | `enabled` | no (default `true`) | arming flag |
| `trailing` | `enabled` | **yes** | arming flag |
| | `trigger_r` | yes | profit, in R, at which trailing starts |
| | `distance_r` | yes | trail distance in R; must be < `trigger_r` |
| `scale_out` | `enabled` | **yes** | arming flag |
| | `trigger_r` | yes | profit, in R, at which the partial exit fires |
| | `fraction` | yes | fraction of the position closed, [0.1, 0.9] |

`break_even.enabled` is optional and defaults to enabled, because historical
configs never carried the flag. `trailing` and `scale_out` require it
explicitly.

One component owns trade management (`PositionManager`). The paper trader's own
ladder is disabled in the same step, so a stop never moves twice.

### Structural exits

Optional fourth block. A third, parallel close path on an open position —
alongside the venue's stop/target and the R-multiple ladder above. It closes when
the market structure that justified the entry disappears, not when a price level
is reached. The fixed stop is still the floor: a structural close is an early
way out, never a replacement for the stop, and whoever closes first wins.

```json
"structural_exit": {
  "enabled": true,
  "min_holdtime_ms": 180000,
  "rules": [
    {"name": "MfeGiveback",    "enabled": true,  "giveback_r": 1.0},
    {"name": "TimeStop",       "enabled": true,  "max_hold_ms": 3600000},
    {"name": "ImbalanceBreak", "enabled": false, "ratio": 2.0, "min_levels": 2}
  ]
}
```

| key | required | meaning |
| --- | -------- | ------- |
| `enabled` | no (default `false`) | master switch; absent or false leaves every existing config unchanged |
| `min_holdtime_ms` | **yes when enabled** | minimum age of the position, in ms, before any rule may close it; must be `> 0` |
| `rules` | **yes when enabled** | at least one entry with a non-empty `name` that is not disabled |

| rule | evaluated on | params | closes when |
| ---- | ------------ | ------ | ----------- |
| `MfeGiveback` | trade | `giveback_r` | the trade gives back that many R of its best favourable excursion, after having actually earned it |
| `TimeStop` | trade | `max_hold_ms` | the position is older than this |
| `ImbalanceBreak` | book | `ratio`, `min_levels` | the top-`min_levels` depth ratio in the position's favour reached `ratio` and has since fallen below it |
| `DeltaFlip` | trade | `min_delta` | the live bar's cumulative delta has flipped against the position by at least this many base-asset units |
| `SpoofPull` | book | `drop_pct`, `min_qty` | a level of at least `min_qty` on the position's side lost `drop_pct` of its size, or disappeared |
| `DeltaDivergence` | trade | `lookback` | price and cumulative delta have diverged against the position over the window |

Book rules run on book frames; trade rules run on prints, ahead of the
break-even/trailing/scale-out ladder. `min_holdtime_ms` is enforced once,
centrally — no rule, including a book rule, can fire before it has elapsed.

The recorded exit reason names the rule: `struct_MfeGiveback`,
`struct_TimeStop`, `struct_ImbalanceBreak`, `struct_DeltaFlip`,
`struct_SpoofPull`, `struct_DeltaDivergence`. A close from this block is
therefore distinguishable from `stop_loss`, `take_profit`, `trailing`,
`scale_out` and `prop_guard` in the trade log.

Validation is split the same way the trigger pipeline is. At load: `enabled`
without a positive `min_holdtime_ms`, an empty `rules`, or an all-disabled
`rules` list aborts. At build: an unknown rule `name`, or a missing/mistyped
param on an *enabled* rule, aborts. A disabled rule's params are never read, so
a typo inside one cannot fail a run.

A rule param may be a grid node, so a sweep can walk `giveback_r` or
`max_hold_ms` like any other number; the scanner prunes a block with
`enabled: false` like any other disabled component.

Only `Close` is implemented. The close flattens through the same
`ClosePartial(1.0)` path the prop guard uses; a `Reduce` or `TightenStop` from a
future rule is logged and ignored, not silently applied.

The guard's trade clock comes from the broker: `IBroker::GetPositionEntryTime()`
reports when the open position was filled. `PaperTrader` answers it, so a
backtest and a paper run measure from the real entry even across a restart. A
broker that answers 0 (unknown) falls back to the first tick the guard sees, and
`min_holdtime_ms` and `TimeStop` then measure from process start — which delays
an exit, never rushes one.

## `prop`

Optional. Funded-account survival rules, enforced independently of `sl_pct`:
per-trade risk and account lifetime limits are different questions.

| key | default | meaning |
| --- | ------- | ------- |
| `enabled` | `false` | master switch |
| `max_daily_drawdown_pct` | — | daily limit, fraction in (0, 1] |
| `max_total_drawdown_pct` | — | overall limit, fraction in (0, 1] |
| `warn_fraction` | — | portion of a limit at which entries stop, (0, 1) |
| `daily_reset_hour_utc` | — | 0–23 |
| `initial_balance` | — | starting equity for the limits |
| `flatten_on_breach` | — | close open positions on breach |

States: unarmed (no equity observed yet — entries are refused rather than traded
blind), normal, warning (no new entries, open trades still managed), daily
halted, total halted. Limits are validated only when `enabled` is true.

Equity is the closed balance plus the floating leg of the open position, re-read
before every print **and** on every book tick. A quiet book can therefore halt
the guard before the next trade prints. On the transition into a breach the open
position is closed once with the exit reason `prop_guard`, so a halt shows up in
the trade log as an ordinary exit rather than an unexplained flat.

A missing `prop.initial_balance` is **not** inherited from the root
`initial_balance` on the backtest paths: grid search, WFA and replay seed the
guard from the paper account's compile-time default. Set it explicitly whenever
the funded account is smaller, or the guard measures drawdown against a larger
base than the account it is modelling — and a config that halts on the venue
will look clean in every backtest.

## `startup_probe`

Optional. The data-integrity gate that runs before a live or paper run arms.

| key | default | meaning |
| --- | ------- | ------- |
| `preset` | `development` | `development` or `production` |
| `min_trades_per_min` | preset | sustained trade arrival rate; `0` disables |
| `min_l2_frames_per_min` | preset | sustained book frame rate; `0` disables |
| `min_duration_ms` | preset | soak length |
| `timeout_ms` | preset | deadline; must be ≥ `min_duration_ms` |
| `max_reconnects` | preset | tolerated reconnects during the soak |
| `min_l2_depth` | preset | levels per side that must be present |
| `on_failure` | `abort` | `abort` or `warn` |

The presets differ only in their numbers:

| field | `development` | `production` |
| ----- | ------------- | ------------ |
| `min_trades_per_min` | 60 | 60 |
| `min_l2_frames_per_min` | 120 | 300 |
| `min_l2_depth` | 2 | 2 |
| `min_duration_ms` | 5000 | 60000 |
| `timeout_ms` | 30000 | 90000 |
| `max_reconnects` | 0 | 1 |

`production` does not raise the depth requirement: depth is a property of the
strategy, not of how long you soak.

Explicit keys override the preset. Rates, not counts: a fixed count is either
too strict at 03:00 or too lax at the open. A positive rate also requires at
least one frame, so a silent channel cannot pass. The required count is derived
from the elapsed time on every evaluation, so the per-channel report shows a
target that grows during the soak.

The rate is cumulative from the start of the probe, not windowed: an early burst
can carry a channel through a later silence.

`min_l2_depth` is a capability check as much as a frame check. The venue's
profile declares how many book levels its feed carries, and asking for more than
that fails before any frame is examined. A shallow venue therefore cannot pass
by delivering perfectly valid snapshots.

`min_l2_depth` must match what your strategy actually indexes. `SpoofHunter`
always aggregates five levels per side; `StackedImbalance.levels` is a stack
count, not a depth. Depth 5 is the value that matches a `SpoofHunter` config.

`on_failure: "warn"` reconnects without the gate. It is a debugging escape
hatch, it is logged as a warning, and it removes exactly the protection the gate
exists to provide.

## Root keys

| key | default | meaning |
| --- | ------- | ------- |
| `initial_balance` | `10000.0` | starting capital, consumed by `--paper` only. Backtests, WFA and replay use the paper account's compile-time default so their results stay comparable, and they do not pass this value to the prop guard either. |
