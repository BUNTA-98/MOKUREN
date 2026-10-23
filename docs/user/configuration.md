# Configuration

One JSON file describes one run. The repository ships no configs: they are
per-user files, passed with `--config=<path>` on every mode except the bare TUI.

## Reading rules

- Unknown keys are ignored. A misspelled key is therefore silent, so compare
  against the skeleton below rather than trusting a config to fail loudly.
- Sections that change what a run does are read strictly: a missing or
  mistyped key aborts with `config error: ...` naming the full path
  (`risk.sl_pct`). There is no default for the venue, for take-profit, or for
  any trade-management level.
- Most numbers may be JSON numbers or numeric strings (`"0.002"`). Exceptions:
  - the R levels (`risk.tp_r`, `trade_management.*.trigger_r`,
    `target_r`, `distance_r`) must be JSON numbers — a quoted one aborts with
    "requires '<key>'";
  - `prop.daily_reset_hour_utc` must be a JSON integer, otherwise it is ignored
    and the default survives;
  - a grid node needs numeric `min`/`max`/`step`, and a `val` that is a number
    or a numeric string.
- Booleans may be `true/false`, `"true"/"false"/"1"/"0"`, or `1/0`.
- Errors carry one of two prefixes: `config error:` (load) and
  `AppConfig validation failed:` (bounds). Both are fatal.

## Units

| quantity | unit | example |
| -------- | ---- | ------- |
| `sl_pct`, `tp_pct`, `risk_per_trade_pct`, fees, slippage | fraction | `0.002` = 0.2% |
| `*_r` levels | multiple of 1R = `sl_pct` | `1.0` = 1R |
| `prop` limits, `warn_fraction` | fraction | `0.04` = 4% |
| `interval_ms`, `macro_interval_ms`, `*_ms` | milliseconds | `60000` = 1 min |
| `min_distance_dollars` | quote currency | `10.0` = 10 USD |
| `tick_size` | price | `0.1` |

## Where a value may live

A few keys are accepted in more than one place. Precedence, highest first:

1. `api_key`, `api_secret`, `is_testnet` — each is read from the root first. If
   the root carries neither `api_key` nor `api_secret`, all three are read from
   an `exchange` object instead, or else from `environment`. A root `api_key`
   with a nested `api_secret` does not mix: the root wins for both.
2. `exchange` — root string, `exchange.name` / `exchange.venue`, then
   `environment.exchange`.
3. `category`, `symbol`, `allow_mainnet` — root, then the `venue` object. Root
   wins when present.
4. `taker_fee_pct`, `slippage_pct` — `execution`, then `environment` (legacy).
5. `triggers`, `filters` — root, then `strategy.triggers` / `strategy.filters`.
   `risk.modules` only at its own path.

`venue.environment` (`mainnet`, `testnet`, `demo`) supersedes the legacy
`is_testnet` boolean; a bool cannot express `demo`.

## Disabling a component

Any entry in `triggers`, `filters` or `risk.modules` is skipped when it carries
`enabled`, `active` or `isActive` set to `false`, `"false"`, `"0"`, `0`, or
`0.0`. Absent means enabled.

If every trigger is disabled the run aborts (`no active triggers`). An unknown
component name is a hard error, never a no-op.

## Grid objects

For grid search a numeric value may be replaced by an object:

```json
{"min": 1.0, "max": 3.0, "step": 0.5, "val": 1.5}
```

`min`/`max`/`step` define the sweep; `val` is the value used outside a sweep.

- The live path collapses every grid object to its `val`. `val` must be a
  number or a numeric string; a node without one is left as an object and the
  numeric readers silently fall back to their defaults.
- `min > max`, a negative `step` or a non-finite bound is a `grid error`.
  `step: 0` is silently rewritten to `1.0` instead of being rejected.
- A grid node inside a component with `enabled: false` is not swept.

## Complete skeleton

Every block below is optional unless marked required.

```json
{
  "exchange": "bybit",

  "api_key": "…",
  "api_secret": "…",

  "initial_balance": 10000.0,

  "venue": {
    "environment": "mainnet",
    "category": "linear",
    "symbol": "BTCUSDT",
    "position_mode": "one_way",
    "allow_mainnet": true
  },

  "environment": {
    "interval_ms": 60000,
    "macro_interval_ms": 900000,
    "tick_size": 0.1,
    "vwap_reset_hour": 0,
    "max_cores": 4
  },

  "execution": {
    "taker_fee_pct": 0.0004,
    "slippage_pct": 0.0002
  },

  "risk": {
    "sl_pct": 0.002,
    "tp_r": 2.0,
    "risk_per_trade_pct": 0.01,
    "max_daily_loss": 400.0,
    "min_distance_dollars": 0.0,
    "modules": [
      {"name": "SinglePositionLock"},
      {"name": "MaxStopDistanceLock", "max_stop_pct": 0.015},
      {"name": "MaxLeverageLock", "max_leverage": 10.0},
      {"name": "AntiRevengeLock", "cooldown_ms": 1800000}
    ]
  },

  "trade_management": {
    "break_even": {"trigger_r": 1.0, "target_r": 0.2, "enabled": true},
    "trailing":   {"enabled": true, "trigger_r": 1.5, "distance_r": 0.5},
    "scale_out":  {"enabled": false, "trigger_r": 1.0, "fraction": 0.5}
  },

  "triggers": [
    {"name": "StackedImbalance", "ratio": 1.5, "levels": 2},
    {"name": "DeltaAbsorption", "delta": 1.0}
  ],

  "filters": [
    {"name": "RVOL", "threshold": 1.5, "lookback": 20}
  ],

  "prop": {
    "enabled": true,
    "max_daily_drawdown_pct": 0.04,
    "max_total_drawdown_pct": 0.06,
    "warn_fraction": 0.8,
    "daily_reset_hour_utc": 0,
    "initial_balance": 10000.0,
    "flatten_on_breach": true
  },

  "startup_probe": {
    "preset": "production",
    "min_l2_depth": 5,
    "on_failure": "abort"
  }
}
```

Required blocks: `environment`, `exchange`, `execution`, `risk`,
`trade_management` (all of `break_even`, `trailing`, `scale_out`), and at least
one active trigger. Details per block are in [Engine settings](config-engine.md)
and [Components](config-components.md).

## Valid ranges

Checked when the config is loaded.

| key | accepted |
| --- | -------- |
| `exchange` | `bybit`, `hyperliquid` |
| `venue.category` | `linear`, `spot`, `inverse` |
| `venue.position_mode` | `one_way`, `hedge` |
| `venue.symbol` | non-empty |
| `venue.environment` | `mainnet` requires `allow_mainnet: true` (bybit) |
| `environment.interval_ms` | > 0 |
| `environment.macro_interval_ms` | > 0 and ≥ `interval_ms` |
| `environment.tick_size` | > 0 |
| `environment.vwap_reset_hour` | 0–23 |
| `execution.taker_fee_pct`, `slippage_pct` | [0.0, 0.1) |
| `risk.sl_pct`, `risk.tp_pct`, `risk_per_trade_pct` | (0.0, 0.1) |
| `risk.min_distance_dollars` | ≥ 0, finite |
| `risk.max_daily_loss` | > 0 |
| `initial_balance` | > 0, finite |
| `trade_management.*.trigger_pct` | (0.0, 0.1) |
| `break_even.target_pct` | [0.0, 0.1) |
| `trailing.distance_pct` | (0.0, 0.1), and < `trigger_pct` when trailing is enabled |
| `scale_out.fraction` | [0.1, 0.9] |
| `startup_probe.min_l2_depth` | ≤ the engine's maximum book depth |
| `startup_probe.timeout_ms` | ≥ `min_duration_ms`; both ≥ 0 |
| `prop.*_drawdown_pct` | (0.0, 1.0] when prop is enabled |
| `prop.warn_fraction` | (0.0, 1.0) when prop is enabled |
| `prop.daily_reset_hour_utc` | 0–23 |
| `triggers` | at least one entry, each with a non-empty `name` |
| `startup_probe.preset` | `development`, `production` |

## Mistakes that are not caught

- A misspelled key is ignored, and the run uses the default or fails on the
  required sibling instead.
- `"enabled": "False"` — with a capital F — is honoured by the factory but not
  by the grid scanner, so a sweep can include a component that never runs.
- A grid object whose `val` is missing is not collapsed. Live mode then reads
  an object where it expects a number and falls back to the default.
- `risk.modules` empty means no locks at all: no position lock, no stop-width
  limit, no cooldown. Nothing is added implicitly.
