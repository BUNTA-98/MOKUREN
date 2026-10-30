# Live test: bybit market data, blofin demo execution

This run prices a bybit **mainnet** book and sends orders to a blofin **demo**
account. It exists to prove two things at once:

1. the bybit streamer produces signals the engine accepts (the startup gate
   passes on bybit's own channels), and
2. the blofin adapter can take a signal all the way to a resting entry plus a
   standalone TP/SL order.

It is not a PnL test. The book you measure against and the book you fill on are
different venues in different worlds, so stops, targets and slippage are the
execution venue's, not the market's. The factory logs that as a `WARN` on every
startup; read it as the caveat it is, not as a fault.

## 1. Prerequisites

- A blofin **demo** account. `BloFinBroker` refuses any other environment at
  construction (`spec.env != Env::Demo`), so a mainnet key aborts at startup
  rather than trading.
- A **demo** API key, not a live one. Blofin keeps two separate key stores
  behind two separate hosts: a live key is minted on the main site and only
  signs against `openapi.blofin.com`, while a demo key is minted from the
  venue's demo-trading site and only signs against
  `demo-trading-openapi.blofin.com`. `venue.environment: "demo"` selects the
  demo host, so a live key fails at `login failed` even though the key itself
  is perfectly valid. Create the key while logged into the demo account.
- Trading permission on that key, plus the **passphrase** set when it was
  created. Blofin signs three credentials; a missing passphrase surfaces as
  `login failed`, not as a config error.
- A funded demo account. `ReconcileAccount` throws `could not read account
  balance at startup` on zero equity and refuses to trade blind. Demo accounts
  normally arrive with virtual USDT; an empty one fails in a way that reads
  like a permissions bug, so check the balance before suspecting the key.
- The account must be `net_mode` (one-way) and `cross` margin. The adapter
  refuses to start otherwise: it sends `positionSide: "net"` and
  `marginMode: "cross"`, and a wrong mode silently turns `reduceOnly` into a
  no-op, which breaks every close.
- **Start flat.** `ReconcileAccount` adopts whatever is open, but an adopted
  position carries no 1R, so `PositionManager`'s ladder cannot arm and the
  position runs unprotected. The adapter logs this as an `ERROR` naming the
  size, but it will not close it for you. A funded balance that reads a little
  under what you deposited is often a stray position, so check the venue's
  positions page rather than assuming a fee.

## 2. Credentials

Do not paste keys into `configs/bybit_to_blofin.json`. The loader reads them
from the environment when the config leaves them empty:

```bash
export BLOFIN_API_KEY="..."
export BLOFIN_API_SECRET="..."
export BLOFIN_API_PASSPHRASE="..."
```

Names are derived from the **execution** venue, so a box that also carries a
bybit account can hold both sets without them colliding (`BYBIT_API_KEY`,
`BYBIT_API_SECRET`). Values written in the config always win over the
environment.

## 3. What the config says

| Key | Value | Why |
| --- | --- | --- |
| `exchange` | `blofin` | the **execution** venue: keys and orders |
| `venue.environment` | `demo` | required by the broker's mainnet guard |
| `venue.data_exchange` | `bybit` | the **feed** venue |
| `venue.data_environment` | `mainnet` | bybit's real book; needs no `allow_mainnet`, because that flag only gates a mainnet **account** |
| `venue.symbol` | `BTCUSDT` | the config's pair name; the blofin broker translates it to `BTC-USDT` on the wire |
| `venue.lot_step` | `0.001` | 1 blofin contract is 0.001 BTC on BTC-USDT |
| `startup_probe.on_failure` | `abort` | fail closed. `warn` starts ungated and you would never learn whether the feed was the problem |
| `prop.enabled` | `false` | a demo equity that does not match `initial_balance` trips the total-drawdown guard on the first tick and flattens everything, which looks exactly like a broken broker |

Turn `prop.enabled` back on only after setting `initial_balance` to the demo
account's actual equity. Drawdown is a fraction of that base, so a $10k demo
account measured against a $25k base is already -60% and `TotalHalted` before
the first signal.

`initial_balance` feeds three different consumers, and only one of them is
live sizing:

- **live sizing** reads `broker->GetBalance()` (REST-primed, then refreshed by
  the wallet stream) and ignores `initial_balance` entirely;
- **`--paper`/scan/wfa** seed the `PaperTrader` from it, so a shadow run sizes
  against this number. A mismatch here makes the shadow run size differently
  from live — compare the two only once they agree;
- **the prop guard** anchors its limits on it, passed as `account_baseline`.

The guard cross-checks the two at startup. `prop guard: equity base …
disagrees with the account's current equity …` means `initial_balance` is not
the funded figure; the run still sizes correctly, but every prop limit would be
measured against capital the account never held. The account may read slightly
under what you funded (a fee, or a position already open on the demo page), so
set the base to the funded amount and let a sub-percent gap pass.

## 4. Running it

Probe first. It signs nothing, so it works before the keys are installed, and
it proves the bybit feed alone:

```bash
./build/mokuren --probe --config=configs/bybit_to_blofin.json --debug
```

A healthy probe prints one `ok` row per channel and `probe: bybit passed in …
ms`. It judges bybit's profile (the feed's venue), not blofin's — a blofin
profile would demand channels bybit does not publish and fail for the wrong
reason.

Then shadow, which streams live bybit data into the local PaperTrader and sends
nothing:

```bash
./build/mokuren --paper --config=configs/bybit_to_blofin.json --debug
```

Finally the real test:

```bash
./build/mokuren --live --config=configs/bybit_to_blofin.json --debug
```

Do not expect a signal immediately. Entries are evaluated on the 1-minute
aggregator, so the first candle closes up to 60 s after start, and the enabled
`MacroTrend` filter reads the 5-minute aggregator with `lookback: 1` — it has
nothing to judge until two 5-minute candles have closed. A quiet first ten
minutes is the expected shape, not a broken pipeline.

`--probe` proves the feed only. It builds with the shadow broker, so it never
signs a request: a passing probe says nothing about whether the `BLOFIN_*`
variables are exported, whether the passphrase belongs to that key, or whether
the account is net_mode/cross. The `--live` startup sequence checks all three,
and each one aborts before the socket opens.

## 5. What a healthy startup looks like

```
factory: cross-venue run: book from bybit (mainnet), orders on blofin (demo)
factory: bybit data venue=bybit env=mainnet ...
factory: blofin live exec venue=blofin env=demo ...
factory: market data is bybit (mainnet) but orders and fills are priced on blofin (demo) ...
BloFinBroker: clock offset estimate ... ms (from ticker data ts)
BloFinBroker: BTC-USDT tick=... lot=... contract=0.001 maxLev=...
BloFinBroker: startup equity ...
BloFinBroker: account is net_mode/cross -- safe to trade
```

Startup order matters: `SyncClock`, then instruments, then balance, then
position mode. A throw from any of them happens **before** the socket opens and
before any signal can be sized, which is why they are hard failures.

`clock offset estimate ... (from ticker data ts)` is an estimate, not drift.
The ticker's `ts` is the timestamp of the data it describes, so a cached or
quiet feed reports a value seconds old. The balance reply carries its own
per-request `ts`, and that is what replaces the estimate — if the two disagree
by more than a second you get a `WARN` naming both numbers. A large negative
estimate next to an NTP-synced host (`timedatectl status`) is stale data, not a
broken clock; do not "fix" the machine.

The demo host's public ticker is often far staler than the live one (the
estimate has read −52 s here while the venue's own reply put it at −172 ms).
That is expected and harmless: the estimate only has to be close enough for the
first signed request, and every order path signs with the corrected offset.

The routing line names the equity the sizer will use:

```
LiveOrchestrator: routing orders to the live venue adapter (equity=..., primed at startup)
```

It is informational, not a warning: `ReconcileAccount()` read the balance over
signed REST before the socket opened, so sizing works from the first signal and
the private wallet stream only refreshes the number. `(equity=0) -- no balance
was primed at startup` means the startup reconcile was bypassed and nothing
will size until the `account` channel pushes.

A startup equity a little under what you funded is normally a fee, but if it
**moves between restarts** while `ReconcileAccount` adopts no position, the
money is somewhere this adapter does not look: `totalEquity` covers every
instrument, and only the configured symbol is reconciled. Check the venue's
positions page across symbols before blaming the balance reader.

## 6. Failure modes

| Symptom | Meaning |
| --- | --- |
| `probe failed: …`, exit 1 | the bybit feed did not meet the gate. Nothing was traded. Fix the feed, or lower `startup_probe` only if you understand what that disables. |
| `BloFinBroker: login failed` | wrong key, wrong passphrase, or a key from the wrong blofin world |
| `could not read account balance at startup` | the signed GET itself failed — wrong key/passphrase for this host, or no read permission. Nothing downstream ran. |
| `account balance is 0 or unreadable at startup (raw reply: …)` | the GET succeeded (`code: "0"`, `msg: success`) and the reply carried no positive equity — **the key, secret and passphrase are correct**, the account is just empty. Check `totalEquity` and `details[].balance` in the dump: all zeros means fund the demo account on the venue. A positive number under a spelling this reader does not know means the parser needs that key added. |
| … `the COIN-FUTURES wallet holds <n>` | your funds are in the wrong product line: this config trades USDT-margined futures, and the balance query is scoped to `USDT-FUTURES`. Fund the USDT-FUTURES wallet on the demo page rather than the coin-margin one. |
| … `the COIN-FUTURES wallet is empty too` | the account really holds nothing anywhere. Fund it on the venue. |
| `position-mode is '…'` / `margin-mode is '…'` | the account is not net_mode/cross. Fix it on the venue; the adapter will not paper over it. |
| `adopted a {long,short} position size=… with NO stop` | the account was not flat at startup and the adopted position has no live TP/SL. Trade management cannot arm without a 1R. Close it on the venue before continuing. |
| `cannot place a fresh TPSL for …: position direction is unknown` | a stop move arrived before the positions stream reported a direction. Transient — the next tick retries. A steady stream of these means the private positions channel is not delivering. |
| `TP/SL rejected (…) entry is UNPROTECTED -- closing to stay safe` | entry filled, the standalone TP/SL order did not land. The adapter sends a reduceOnly market close immediately. **Check the account**: if the emergency close also failed, a position is open with no stop. |
| entry rejected with `< minSize` | `signal.volume` in BTC is below one contract after rounding. Size up or check `lot_step`. |
| order paths refuse to send | `FetchInstrumentRules` never ran, so `contract_value_` is 0. That is a startup failure, not a runtime one — look higher in the log. |

## 7. Known limits of this run

- **The order path is unproven until a signal fires.** A clean startup proves
  authentication, instrument rules, balance, account mode and the private
  socket. `ProcessSignal`, `order-tpsl`, `amend-tpsl` and `ClosePartial` have
  not run yet — they are the calls with the venue-specific assumptions (size in
  contracts, `size: "-1"` reduceOnly TPSL, the `tpslId` reply shape). The first
  `BloFinBroker: closing N contracts` in a run is the real milestone.
- The stop is not on the matching engine until the second REST call lands. The
  window is bounded by the emergency close, not by the venue.
- `UpdateStopLoss` is debounced to 4 moves/s, above blofin's sustained 3/s
  budget. A position that trails on every tick can still exhaust 30 requests /
  10 s; the debounce bounds a single tick, it does not make the path rate-safe.
- Market orders only. `amend-order` is scoped to a single limit order on this
  venue, which is why the stop is a separate TP/SL object amended via
  `amend-tpsl`.

## 8. Ending the session

ctrl-c sends SIGINT, which the engine turns into `orchestrator.Stop()`. A
clean stop logs `live mode: shutdown requested`, then
`live mode: stopped cleanly`, and exits 0. A non-zero exit means the market
stream aborted or the startup gate failed — read the reason line before
assuming the test "just ended".

Before stopping, look at the account, not at the log:

1. **No open position.** If one is open, the engine was mid-trade. Stopping
   does not close it, and the venue keeps the standalone TP/SL armed, so the
   position stays protected but unattended. Close it on the venue (or with the
   TP/SL left in place) before walking away.
2. **No orphan TP/SL.** A cancelled entry can leave a `tpslId` on the book. If
   the log shows `cancelled orphan TPSL ...`, that is the adapter cleaning up
   correctly; if it shows `cancel-tpsl ... rejected`, check the venue's open
   orders.

Then stop the process. Nothing is sent to the venue during shutdown.

## 9. Committing the config

`configs/bybit_to_blofin.json` carries empty `api_key` / `api_secret` /
`api_passphrase` on purpose; the keys come from the environment. That makes
the file safe to commit and is the reason not to "temporarily" paste them in.

Before committing, confirm nothing secret is staged:

```bash
git status --short
git diff --cached --stat
```

Neither the working tree nor the index should mention a `.env` file, a key, or
a passphrase. `logs/` holds the full trace of the session and should stay
ignored.
