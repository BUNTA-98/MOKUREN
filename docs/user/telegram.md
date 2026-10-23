# Telegram notifications

## How it fits together

The engine does not talk to Telegram. It publishes run events to a local Redis
channel, and `tools/TELEGRAM.py` subscribes to that channel and forwards them
through the Bot API:

```
mokuren --live / --paper  ->  redis pub/sub  ->  tools/TELEGRAM.py  ->  Telegram
```

- The notifier is optional and independent. If it is down, trading continues
  unaffected; nothing is queued and nothing is retried, so those events are
  simply never posted.
- Only the live orchestrator publishes — the same path `--paper` uses. Backtests,
  grid search, walk-forward and replay publish nothing.
- The engine connects to `127.0.0.1:6379` on channel `mokuren:events`. Those are
  compiled in; the notifier has to reach that instance.

The same connection also feeds two Redis *streams* that this setup never reads:
`mokuren:trades` (trade events, capped) and `mokuren:ticker` (last price, at most
one write per second), both intended for a dashboard. They are separate keys, so
a charting problem never explains a missing message, and vice versa.

## 1. Create the bot

1. Message `@BotFather`, send `/newbot`, choose a display name and a username
   ending in `bot`.
2. Copy the token (`123456789:AA…`). Treat it as a password: anyone holding it
   controls the bot.
3. Put the bot where messages should go:
   - **private chat** — open a chat with the bot and send it anything;
   - **group** — add the bot to the group;
   - **channel** — add the bot as an administrator with post permission.

## 2. Get the chat id

Send one message in the target chat, then read the update:

```bash
curl -s "https://api.telegram.org/bot<TOKEN>/getUpdates"
```

The id is `result[].message.chat.id`. Private chats are positive; groups and
channels are negative, channels normally starting with `-100`.

For a public channel, `@channelname` is accepted instead of the numeric id as
long as the bot is an administrator there. Prefer the numeric id: it survives a
rename.

## 3. Configure

Configuration comes from the environment; a `.env` file is loaded first
(`python-dotenv`, searched upward from the working directory).

| variable | default | meaning |
| -------- | ------- | ------- |
| `TELEGRAM_BOT_TOKEN` | — | required; missing exits 1 |
| `TELEGRAM_CHAT_ID` | — | required; missing exits 1 |
| `REDIS_HOST` | `127.0.0.1` | host the engine publishes to |
| `REDIS_PORT` | `6379` | port the engine publishes to |
| `REDIS_CHANNEL` | `mokuren:events` | must match the engine's compiled channel |

`.env` beside the directory you start the script from:

```bash
TELEGRAM_BOT_TOKEN=123456789:AA...
TELEGRAM_CHAT_ID=-1001234567890
```

```bash
chmod 600 .env
```

## 4. Run it

```bash
python3 -m pip install redis requests python-dotenv
python3 tools/TELEGRAM.py
```

On start it prints `[redis] subscribed to 'mokuren:events' on 127.0.0.1:6379`.
After that it is silent on stdout; failures go to stderr.

| situation | behaviour |
| --------- | --------- |
| Redis unreachable or the connection drops | logs the error, retries every 5 s, indefinitely |
| message is not JSON, or not a JSON object | dropped with a stderr line |
| Telegram rejects the message or the network fails | logged to stderr, that message is lost |
| Ctrl-C | exits 0 |

Exit codes: `0` normal shutdown, including Ctrl-C; `1` missing
`TELEGRAM_BOT_TOKEN` or `TELEGRAM_CHAT_ID`.

## Message catalogue

Five actions are published. The table is what the engine sends, not what the bot
renders.

| action | emitted when | keys carried |
| ------ | ------------ | ------------ |
| `SYSTEM_STARTUP` | process start, **before** the startup gate runs | `action`, `msg` |
| `SYSTEM_SHUTDOWN` | a stop is requested, before teardown | `action`, `msg` |
| `TRADE_OPEN` | a new position was established | `action`, `timestamp`, `price`, `size`, `symbol`, `direction`, `entry_price`, `stop_loss`, `take_profit`, `volume` |
| `POSITION_CLOSED` | a position became flat | `action`, `timestamp`, `price`, `size`, `symbol`, plus `pnl`, `exit_reason`, `direction`, `entry_price`, `exit_price` when the trade log has the record |
| `PROP_HALT` | the prop guard breached and is about to flatten | `action`, `timestamp`, `price`, `symbol`, `reason` |

What that means in practice:

- **The two system events carry only a fixed `msg`** (`Engine online` /
  `Engine offline`). The bot looks for `dataset`, `mode` and `reason` in them, so
  those render as `n/a` on every system message. Do not build a "which mode is
  running" alert on `SYSTEM_STARTUP`.
- **`timestamp` is milliseconds since the epoch**, not the ISO string the bot
  formats when it writes the message.
- **`POSITION_CLOSED` can arrive with no `pnl`.** A close booked against an empty
  trade log still publishes the bare event.
- **A scale-out reports the whole trade.** When a position closes in fragments,
  `pnl` is the sum of every record sharing the newest `entry_time` — the trade,
  not the final fragment.
- **Partial closes stay silent.** No `POSITION_CLOSED` is published until the
  position is fully flat.
- **`PROP_HALT` precedes the flatten**, so it carries the price about to be used
  and no `pnl`; the realised result follows as a separate `POSITION_CLOSED`.

Rendering: `TRADE_OPEN` uses a green square for `BUY` and a red one otherwise;
`POSITION_CLOSED` shows a money bag for a non-negative `pnl`; missing fields read
`n/a` and numbers get four decimals. An unknown `action` is posted as
`Event: <action>` followed by the pretty-printed payload, so a new event type
surfaces without touching the script.

Examples:

```json
{"action": "SYSTEM_STARTUP", "msg": "Engine online"}
```

```json
{"action": "TRADE_OPEN", "timestamp": 1761213600000, "price": 61234.5,
 "size": 0.012, "symbol": "BTCUSDT", "direction": "BUY",
 "entry_price": 61234.5, "stop_loss": 61112.0, "take_profit": 61480.0,
 "volume": 0.012}
```

```json
{"action": "POSITION_CLOSED", "timestamp": 1761216060000, "price": 61112.0,
 "size": 0.012, "symbol": "BTCUSDT", "pnl": -12.4, "exit_reason": "stop",
 "direction": "BUY", "entry_price": 61234.5, "exit_price": 61112.0}
```

```json
{"action": "PROP_HALT", "timestamp": 1761228000000, "price": 60800.0,
 "symbol": "BTCUSDT", "reason": "daily drawdown limit"}
```

## Testing without the engine

Publish a frame by hand and watch it arrive:

```bash
redis-cli publish mokuren:events '{"action":"SYSTEM_STARTUP","msg":"Engine online"}'
```

If nothing reaches Telegram, the problem is between the script and the bot. If
it does arrive, the engine side is what to check (`logs/engine.log`, or
`journalctl` for a daemon).

## Running it as a service

The notifier is an ordinary long-running process. Nothing in `deploy/` ships a
unit for it:

```ini
[Unit]
Description=Mokuren Telegram notifier
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/home/you/src/mokuren
ExecStart=/home/you/src/mokuren/.venv/bin/python tools/TELEGRAM.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

`WorkingDirectory` is what lets `load_dotenv()` find `.env`; without it the
process exits 1 immediately.

## Gotchas

**"Engine online" arrives, then the process dies.** The startup event is
published before the stream is dialled and before the gate runs, and that
failure path deliberately flushes the publisher so the last record is not lost.
A run that fails its checks therefore announces itself and then exits. Read the
absence of a later `TRADE_OPEN` as "it never armed", not the startup message.

**A notification never arrived.** The script does not retry. A Telegram 429, a
5xx or a network blip loses that event permanently; restarting the notifier does
not replay it.

**Some events arrive, others do not.** Messages go out with
`parse_mode: Markdown` and free-form values (`reason`, `exit_reason`, `symbol`)
are interpolated unescaped. A value containing `_`, `*`, `` ` `` or `[` makes
Telegram answer 400; the script logs it and drops the message. A halt reason or
venue error string with an underscore is the usual culprit.

**Nothing arrives after a redeploy.** Check that a single notifier owns the
channel, and that `REDIS_CHANNEL` matches the key the engine publishes to.

**A `--paper` run posts identically.** Both live modes go through the same
orchestrator and the startup payload carries no `mode`, so a paper session and a
funded one cannot be told apart in the chat. Use a separate token or channel per
environment if that distinction matters.

**A halt reads like an ordinary exit in the log.** A prop breach flattens with
the reason `prop_guard`, so the `POSITION_CLOSED` following `PROP_HALT` looks
like any other stop-out unless you check the reason field.

## Related

- [Running as a daemon](deployment.md) — the engine side of the same host.
- [Live and paper](live-and-paper.md) — when the gate runs and what shutdown does.
