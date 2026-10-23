# Config examples

Each example is complete enough to load. Fill in credentials, adjust
`tick_size` to the instrument, and check `min_l2_depth` against the triggers you
use.

## Paper: order-flow scalp

Direction from stacked footprint imbalances, absorption as a second entry,
volume and trend as vetoes. Three locks, break-even at 1R.

```json
{
  "exchange": "bybit",
  "venue": {"environment": "mainnet", "category": "linear",
            "symbol": "BTCUSDT", "position_mode": "one_way",
            "allow_mainnet": true},
  "environment": {"interval_ms": 60000, "macro_interval_ms": 900000,
                  "tick_size": 0.1, "vwap_reset_hour": 0},
  "execution": {"taker_fee_pct": 0.0004, "slippage_pct": 0.0002},
  "risk": {
    "sl_pct": 0.002, "tp_r": 2.0, "risk_per_trade_pct": 0.01,
    "max_daily_loss": 400.0,
    "modules": [
      {"name": "SinglePositionLock"},
      {"name": "MaxStopDistanceLock", "max_stop_pct": 0.004},
      {"name": "MaxLeverageLock", "max_leverage": 5.0}
    ]
  },
  "trade_management": {
    "break_even": {"trigger_r": 1.0, "target_r": 0.2},
    "trailing":   {"enabled": false, "trigger_r": 1.5, "distance_r": 0.5},
    "scale_out":  {"enabled": false, "trigger_r": 1.0, "fraction": 0.5}
  },
  "triggers": [
    {"name": "StackedImbalance", "ratio": 1.5, "levels": 3},
    {"name": "DeltaAbsorption", "delta": 15.0}
  ],
  "filters": [
    {"name": "RVOL", "threshold": 1.2, "lookback": 20},
    {"name": "VwapTrend"}
  ],
  "startup_probe": {"preset": "production", "min_l2_depth": 5}
}
```

## Paper: spoof hunter

Tick-driven, so the startup soak matters more than the candle interval. Probe
depth is 5 because `SpoofHunter` always reads five levels.

```json
{
  "exchange": "bybit",
  "venue": {"environment": "mainnet", "category": "linear",
            "symbol": "ETHUSDT", "allow_mainnet": true},
  "environment": {"interval_ms": 60000, "macro_interval_ms": 900000,
                  "tick_size": 0.01, "vwap_reset_hour": 0},
  "execution": {"taker_fee_pct": 0.0004, "slippage_pct": 0.0002},
  "risk": {
    "sl_pct": 0.003, "tp_r": 1.5, "risk_per_trade_pct": 0.005,
    "max_daily_loss": 200.0,
    "modules": [
      {"name": "SinglePositionLock"},
      {"name": "AntiRevengeLock", "cooldown_ms": 600000}
    ]
  },
  "trade_management": {
    "break_even": {"trigger_r": 1.0, "target_r": 0.1},
    "trailing":   {"enabled": true, "trigger_r": 1.2, "distance_r": 0.4},
    "scale_out":  {"enabled": false, "trigger_r": 1.0, "fraction": 0.5}
  },
  "triggers": [
    {"name": "SpoofHunter", "lookback_ms": 750,
     "min_wall_qty": 120.0, "drop_threshold": 0.85}
  ],
  "filters": [
    {"name": "TimeOfDay", "start_h": 7, "start_m": 0, "end_h": 20, "end_m": 0}
  ],
  "startup_probe": {"preset": "production", "min_l2_depth": 5,
                    "min_trades_per_min": 30}
}
```

Run it with `TZ=UTC` if the time window must match a UTC schedule.

## Grid: sweep the stop and the stack depth

Values may be grid objects; `val` is used outside a sweep.

```json
{
  "exchange": "bybit",
  "venue": {"environment": "testnet", "category": "linear",
            "symbol": "BTCUSDT"},
  "environment": {"interval_ms": 60000, "macro_interval_ms": 900000,
                  "tick_size": 0.1, "vwap_reset_hour": 0},
  "execution": {"taker_fee_pct": 0.0004, "slippage_pct": 0.0002},
  "risk": {
    "sl_pct": {"min": 0.001, "max": 0.004, "step": 0.001, "val": 0.002},
    "tp_r": 2.0, "risk_per_trade_pct": 0.01, "max_daily_loss": 400.0,
    "modules": [{"name": "SinglePositionLock"}]
  },
  "trade_management": {
    "break_even": {"trigger_r": 1.0, "target_r": 0.2},
    "trailing":   {"enabled": false, "trigger_r": 1.5, "distance_r": 0.5},
    "scale_out":  {"enabled": false, "trigger_r": 1.0, "fraction": 0.5}
  },
  "triggers": [
    {"name": "StackedImbalance", "ratio": 1.5,
     "levels": {"min": 2, "max": 4, "step": 1, "val": 2}}
  ],
  "filters": [],
  "prop": {"enabled": true, "max_daily_drawdown_pct": 0.04,
           "max_total_drawdown_pct": 0.06, "warn_fraction": 0.8,
           "daily_reset_hour_utc": 0, "initial_balance": 10000.0,
           "flatten_on_breach": true}
}
```

Two sweep parameters give 4 × 3 = 12 runs. `--live` and `--paper` collapse both
to their `val`.
