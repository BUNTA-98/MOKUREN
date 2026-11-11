# API keys for `configs/hyro_split.json` (bybit demo + testnet)

This runbook covers exactly one thing: how to get a bybit API key into this
engine so `configs/hyro_split.json` can place orders, and how to keep that key
out of git. It does not explain the strategy.

The config reads **mainnet market data** and trades on a **simulated account**.
That split is the point: the strategy sees a real book, the orders cost nothing.

```
data   (venue.data_environment) -> mainnet   public websocket, no key needed
exec   (venue.environment)      -> demo      api key, orders, fills, equity
```

---

## 0. Where keys are allowed to live

`AppConfig::Load` reads credentials from the **first** of these that contains an
`api_key`:

| location                          | example                                        |
|-----------------------------------|------------------------------------------------|
| root of the config                | `{"api_key": "...", "api_secret": "..."}`      |
| `exchange` object                 | `{"exchange": {"name": "bybit", "api_key": ...}}` |
| `environment` object              | `{"environment": {"api_key": "...", ...}}`     |

`configs/hyro_split.json` uses the root form with empty strings, so `--live`
fails `Validate()` immediately with `api_key must not be empty when exchange is
"bybit"` — that is the intended fail-closed behaviour, not a bug.

**There is no environment-variable or secret-manager support today.** The key
must be in a file the process can read. If you need env-var indirection, that is
a change to `src/config_parser.cpp`, not a config change.

---

## 1. Demo vs testnet — pick one, they are different worlds

|                | demo                                    | testnet                                |
|----------------|-----------------------------------------|----------------------------------------|
| where keys come from | bybit main site, `Demo Trading` → API Management | `testnet.bybit.com` → API Management |
| market data    | production mainnet book (realistic)     | separate testnet book (thin, synthetic) |
| balance        | virtual, periodically reset             | virtual, reset on request              |
| `venue.environment` | `"demo"`                           | `"testnet"`                            |
| `allow_mainnet`     | `false`                            | `false`                                |

A key from one world returns **retCode 10003 / "invalid api key"** in another.
They are not interchangeable, and no fallback exists on purpose.

`configs/hyro_split.json` ships with `"environment": "demo"`. To use testnet,
change that single line:

```json
"venue": { "environment": "testnet", "data_environment": "mainnet", ... }
```

Do **not** change `data_environment`. The mainnet feed needs no key, and it is
what makes the demo/testnet fills comparable to a real market.

---

## 2. Create the key

### demo account
1. Log in to bybit (main site), open **Demo Trading**.
2. Inside the demo environment, open **API Management** / **API Keys**.
3. Create a key with permissions: **Read** + **Trade** (contract/derivatives).
   Leave **Withdraw** off — nothing in this engine ever withdraws.
4. If the UI offers an IP whitelist, set your server's egress IP. This one step
   is the difference between a leaked key being useless and being a problem.

Note: a demo key is created *while in demo mode*. If the UI does not show API
management, your account does not have demo trading enabled yet — enable it
first, then create the key.

### testnet account
1. Go to `testnet.bybit.com` and register (separate account from mainnet).
2. **API Management** → create key, same permissions: **Read** + **Trade**, no
   withdraw, IP whitelist if available.
3. Fund the testnet account from the testnet faucet, otherwise the position
   sizer has no equity to size against.

---

## 3. Put the key in a file that git ignores

The committed `configs/hyro_split.json` stays secret-free. Copy it for the real
run:

```bash
cp configs/hyro_split.json configs/hyro_split.local.json
chmod 600 configs/hyro_split.local.json
```

Edit `configs/hyro_split.local.json` and fill only the two fields:

```json
"api_key": "XXXXXXXXXXXXXXXXXXXX",
"api_secret": "YYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYY",
```

Then make sure the copy never lands in a commit:

```bash
grep -q 'configs/\*.local.json' .gitignore || echo 'configs/*.local.json' >> .gitignore
git status --short
```

`git status` must not list `configs/hyro_split.local.json`. If it does, fix
`.gitignore` before the first commit — a key that reaches history stays in
history.

On a server, also run the engine as a dedicated user or a systemd unit whose
`EnvironmentFile` points at the key file rather than passing it on the command
line (the full argv is written into `logs/engine.log` at startup).

---

## 4. Verify before you trust it

Run the three steps in this order. Each one is a strictly larger claim than the
previous one, and each failure is cheaper than the next.

### a) probe — no key involved at all

```bash
./build/mokuren --probe --config=configs/hyro_split.json
```

This dials the **mainnet public feed** and runs the startup gate. It signs
nothing, so an empty `api_key` is fine here. Expected: one row for `trades`, one
for `l2`, then `probe: bybit passed in <ms>`. A failure here means the network or
the venue, never the key.

### b) shadow — keys still not used

```bash
./build/mokuren --paper --config=configs/hyro_split.json
```

Real mainnet data, local fills, no order leaves the process. This proves the
strategy, the sizer and the prop guard behave before a venue sees an order. Look
for:

```
factory: bybit data env=mainnet category=linear symbol=BTCUSDT ...
factory: bybit shadow (paper execution) ...; no orders will be sent
```

### c) live — orders reach the demo account

```bash
./build/mokuren --live --config=configs/hyro_split.local.json
```

Expected startup lines:

```
factory: bybit data env=mainnet category=linear symbol=BTCUSDT rest=https://api.bybit.com ...
factory: bybit live exec env=demo category=linear symbol=BTCUSDT rest=https://api-demo.bybit.com ...
factory: market data is mainnet but orders and fills are priced on demo; the strategy sees one book and fills on another
```

That `WARN` is not an error. It states the one caveat that changes what the
numbers mean: stop/target distances are measured on mainnet's book, fills happen
on demo's simulated book. Demo tracks mainnet closely; testnet does not.

---

## 5. When it refuses to trade

These are the four failures you will actually hit on a fresh server, in order of
likelihood.

**`api_key must not be empty when exchange is "bybit"`**
You ran `--live` against the template instead of `configs/hyro_split.local.json`.
Working as intended.

**`probe ... FAILED: l2 < n/min`**
The feed is fine but the gate is strict. `configs/hyro_split.json` uses
`startup_probe.preset: "development"`. If you raised `min_l2_frames_per_min`, a
quiet overnight session fails a check it would pass at the open. Rates, not
counts, for exactly this reason.

**Engine starts, logs `factory: bybit live exec ...`, then never trades**
Check `PropGuard`. On the live path its equity base comes from the **active
broker**, and `GetBalance()` is `0.0` until the private websocket delivers the
first wallet frame. Until then the guard reports `Unarmed` and refuses entries —
correctly: it will not size a position against equity it has not seen. If that
state never clears, the private stream is not authenticating, which brings us to:

**retCode 10003 / `invalid api key` in the log**
The key is from the wrong world. A mainnet key with `"environment": "demo"` and a
demo key with `"environment": "testnet"` both look identical from here. Recreate
the key in the world named by `venue.environment` and change nothing else.

---

## 6. What this config will and will not tell you

It will tell you: the venue connection works, the split resolution works, orders
are accepted, stops and targets are attached, the prop guard is armed and counts
down, and the trade log is written.

It will not tell you: whether the strategy is profitable. StackedImbalance here
is a smoke-test trigger with deliberately loose thresholds, `filters` is empty,
and fills are simulated on a different book than the one that produced the
signal. Treat a green run as "the plumbing is sound", nothing more.
