# Components

Everything listed here goes into one of three arrays:

```json
"triggers":     [{"name": "…", "…": 0}],
"filters":      [{"name": "…", "…": 0}],
"risk": {"modules": [{"name": "…", "…": 0}]}
```

An unknown `name` aborts the run. A component carrying `enabled`, `active` or
`isActive` set to false — `false`, `"false"`, `"False"`, `"FALSE"`, `"0"`, `0`
or `0.0` — is skipped. The grid scanner honours a narrower set (no `"False"`,
no `0.0`), so use lowercase `false` or `0` in a swept config.

## Pipeline order

```
candle/tick → triggers → filters → risk modules → sizing → execution → management
```

- A trigger states a direction. Levels come from `risk.sl_pct` and `tp_r`, so a
  trigger never has to know the account's risk.
- Filters veto a signal the trigger produced. They never create one.
- Risk modules are evaluated in the order they appear; the first rejection wins.
- Every rejection is logged at trace level. Run with `--debug` to see which veto
  fired and with which values.

## Triggers

More than one trigger is combined with OR: the first one that returns a
direction wins. There is no AND combination available from a config file.

| name | param | type | default | meaning |
| ---- | ----- | ---- | ------- | ------- |
| `StackedImbalance` | `ratio` | number | `1.5` | how much larger one side must be than the other to count as imbalanced |
| | `levels` | int | `2` | minimum number of consecutive imbalanced price levels |
| `DeltaAbsorption` | `delta` | number | `1.0` | minimum absolute cumulative delta, in base units, for a signal |
| `SpoofHunter` | `lookback_ms` | int | `500` | how far back the wall is compared |
| | `min_wall_qty` | number | `30.0` | minimum aggregated size, per side, to call it a wall |
| | `drop_threshold` | number | `0.9` | fraction of the wall that must disappear |

### When each one runs

| trigger | evaluated | reads |
| ------- | --------- | ----- |
| `StackedImbalance` | candle close | footprint of the last bar |
| `DeltaAbsorption` | candle close | footprint of the last bar |
| `SpoofHunter` | every tick | `latest_l2` and the L2 history ring |

A tick-only trigger cannot contribute to a candle evaluation, and (because the
AND combinator is candle-only) a tick trigger belongs at the top level of the
trigger list.

### `StackedImbalance`

Walks the packed footprint and counts how many consecutive price levels have one
side larger than the other by `ratio`. Fires BUY on `levels` or more consecutive
bid-side imbalances, SELL on the mirror. Direction only.

The gap that breaks a run is `tick_size * 1.5`. Set `tick_size` correctly: with
a zero tick the trigger infers one from the footprint, which under-counts
stacked levels.

### `DeltaAbsorption`

Sums `ask_volume - bid_volume` over the footprint. BUY when cumulative delta is
at most `-delta` but the candle closed green; SELL when it is at least `delta`
but the candle closed red. This is the only trigger that also proposes an
entry, a stop (two ticks beyond the candle extreme) and a 2:1 target.

### `SpoofHunter`

Aggregates the top **five** levels per side for a snapshot at least
`lookback_ms` old and for the current snapshot. If a side held at least
`min_wall_qty` and has since lost `drop_threshold` of it, the wall was pulled:
BUY when it was an ask wall, SELL when it was a bid wall.

Five levels is fixed and not configurable. Sizing this trigger therefore means
sizing the feed: if the venue carries fewer than five levels, the empty slots
read as zero and a one-level book looks like a 100% wall removal. Set
`startup_probe.min_l2_depth` to 5 for these configs.

## Filters

| name | param | type | default | vetoes when |
| ---- | ----- | ---- | ------- | ----------- |
| `MinVolume` | `min_volume` | number | `1.0` | last bar volume is below the threshold |
| `RVOL` | `threshold` | number | `1.5` | current volume / average of the previous `lookback` bars is below the threshold |
| | `lookback` | int | `20` | — |
| `ATRChop` | `min_atr` | number | `20.0` | average true range over `lookback` is below the threshold |
| | `lookback` | int | `14` | — |
| `Volatility` | `min_dollar` | number | `150.0` | average high−low over `lookback` is below the threshold |
| | `lookback` | int | `5` | — |
| `MacroTrend` | `lookback` | int | `1440` | a BUY below the SMA, or a SELL above it |
| `VwapTrend` | `require_trend_alignment` | bool | `true` | a BUY below session VWAP, or a SELL above it; `false` disables the filter entirely |
| `POCTrend` | — | — | — | a BUY below the last bar's POC, or a SELL above it |
| `CVDDivergence` | `lookback` | int | `5` | price and cumulative delta disagree over the window |
| `TimeOfDay` | `start_h`, `start_m` | int | `8`, `0` | the bar's local time is outside the window |
| | `end_h`, `end_m` | int | `17`, `0` | — |
| `OrderbookImbalance` | `ratio` | number | `3.0` | the near side is not `ratio` times the far side |

Behaviour worth knowing before you size these:

- All filters except `OrderbookImbalance`, `POCTrend` and `VwapTrend` fail
  closed when there is not enough history. The three exceptions pass on missing
  data, so none of them proves a book exists.
- `TimeOfDay` converts the bar timestamp with the host's local timezone. Use
  `TZ=UTC` for reproducible backtests and to keep a replay consistent with a
  live run on another machine.
- `VwapTrend` compares against the live bar's close, while the others use the
  last closed bar.
- `ATRChop` is the config name for the ATR filter.

## Risk modules

| name | param | type | default | rejects when |
| ---- | ----- | ---- | ------- | ------------ |
| `SinglePositionLock` | — | — | — | a position is already open |
| `MaxStopDistanceLock` | `max_stop_pct` | number | `0.015` | the stop is wider than this fraction of entry, or entry/stop is 0 |
| `MaxLeverageLock` | `max_leverage` | number | `10.0` | notional / balance exceeds this, or entry/volume is 0 |
| `AntiRevengeLock` | `cooldown_ms` | int | `1800000` | a trade closed less than the cooldown ago |

Nothing is added implicitly. An empty `risk.modules` list means no position
lock, no stop-width ceiling and no cooldown.

`MaxDrawdownLock` exists in the codebase but is not constructible from a config
file; `risk.max_daily_loss` is not a substitute on the venue path. Configure the
modules you rely on explicitly.

On the venue path the modules read the exchange's cached account state, which is
empty until the first private websocket frame. Until then `GetBalance()` is 0,
`MaxLeverageLock` rejects everything, and no size is produced — an engine that
appears to ignore signals right after startup is usually this.
