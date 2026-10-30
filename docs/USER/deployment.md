# Deploying Mokuren

Three separate pieces, on purpose:

1. **redis** (`docker-compose.yml`) — transport. Publishes nothing, forgets
   everything: pub/sub has no replay and no persistence.
2. **mokuren** — the engine. Publishes JSON to `mokuren:events` and knows
   nothing about Telegram.
3. **mokuren-telegram** (`tools/TELEGRAM.py`) — subscribes to `mokuren:events`
   and forwards to Telegram.

If nothing is subscribed, events are silently dropped. That is the whole
reason the listener runs as an always-on service instead of being launched per
trading session.

## First-time setup on a new machine

### 1. Packages

`CMakeLists.txt` is authoritative for what's required. `spdlog`,
`nlohmann-json`, `ixwebsocket` and `Catch2` are pulled via `FetchContent`, so
they are **not** installed here — but the first `cmake -B build` needs `git`
and outbound network to fetch them.

```bash
sudo pacman -S --needed base-devel cmake git openssl curl hiredis notcurses \
    docker docker-compose redis \
    python-redis python-requests python-dotenv
```

`redis` is the client (`redis-cli`); the server comes from compose.

### 2. Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

### 3. Redis + Grafana

```bash
sudo systemctl enable --now docker
docker compose up -d
```

Everything is `restart: unless-stopped`, so it comes back after a reboot as
long as the docker daemon is enabled. To run `docker compose` without `sudo`:
`sudo usermod -aG docker "$USER"` and log back in.

### 4. Telegram bot and chat id

Message @BotFather, `/newbot`, copy the token. Then send `/start` to the bot
(or add it to a group). A bot cannot message a chat that never initiated — the
result is a 403 on every send, which looks exactly like silence. Get the chat
id from `curl "https://api.telegram.org/bot<TOKEN>/getUpdates"` —
`result[0].message.chat.id`.

### 5. Secrets

```bash
mkdir -p "$HOME/.config/mokuren"
install -m 600 /dev/null "$HOME/.config/mokuren/telegram.env"
$EDITOR "$HOME/.config/mokuren/telegram.env"
```

```ini
TELEGRAM_BOT_TOKEN=123456:ABC...
TELEGRAM_CHAT_ID=123456789
```

A repo-root or `tools/.env` also works (python-dotenv walks up from the
script), but keep those out of git. `EnvironmentFile=-` in the unit means a
missing file is not a unit failure — it just falls back to `.env`.

### 6. Units

```bash
sudo cp deploy/mokuren-telegram.service deploy/mokuren@.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now mokuren-telegram
```

`mokuren@` instances are added per config below.

### 7. Verify

```bash
systemctl status mokuren-telegram
journalctl -u mokuren-telegram -n 20 --no-pager
redis-cli PUBSUB NUMSUB mokuren:events
```

You want `[redis] subscribed to 'mokuren:events'` in the journal and a
subscriber count of `1`. Anything else is diagnosed under "Troubleshooting".

## Running as a daemon

`deploy/mokuren@.service` is a systemd *template* unit. The instance name after
`@` is substituted for `%i`, which names the config:

```
mokuren@btc_scalp  ->  --config=/home/bunta/src/kaito/configs/btc_scalp.json
```

The `.json` suffix is appended by the unit, so the instance name is the basename
without it.

## The unit as shipped

| field | value |
| ----- | ----- |
| `User` / `Group` | `bunta` |
| `WorkingDirectory` | `%h/src/kaito` — home of the `User=` above |
| `ExecStart` | `%h/src/kaito/build/mokuren --live --config=…/configs/%i.json` |
| `Restart` | `on-failure`, after 5 s |
| `StartLimitIntervalSec` / `StartLimitBurst` | `300` / `5` |
| `TimeoutStopSec` | `30` |
| `StandardOutput` / `StandardError` | `journal` |
| ordering | `After=`/`Wants=` on `network-online.target`, `docker.service`, `mokuren-telegram.service` |

**Edit `User=` and `Group=` before using it.** Every path uses `%h`, the home
directory of that user, and the repo is expected at `~/src/kaito`. An unedited
copy whose home has no `src/kaito` fails with `200/CHDIR`; the restart limiter
then stops it after five attempts instead of looping forever.

## Install

```bash
sudo cp deploy/mokuren@.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now mokuren@btc_scalp
journalctl -u mokuren@btc_scalp -f
```

`journalctl -u mokuren@btc_scalp -n 200 --no-pager` is the fast way to see why a
unit did not stay up.

## Checklist before enabling

1. **The config validates headless.** Run the probe first:
   `./build/mokuren --probe --config=…/configs/btc_scalp.json`. A config that
   fails `AppConfig` validation will fail identically under systemd, and the
   unit will loop.
2. **`WorkingDirectory` is where you want side effects.** `logs/engine.log` and
   `recordings/` are relative to it.
3. **The service user can read the config and write `logs/`.** A key file
   readable only by root produces a credential failure, not a permission error.
4. **`allow_mainnet` is what you mean.** The default is testnet.
5. **The gate can pass at the hour you start it.** `min_trades_per_min` sized for
   the open will fail at 03:00 and take the unit down.

## Things the shipped unit does not do

- **No timezone pin.** `TimeOfDayFilter` converts bar timestamps with the host's
  local timezone, so a `TimeOfDay` window means different hours on different
  machines. Pin it if you rely on one:

  ```ini
  [Service]
  Environment=TZ=UTC
  ```

- **No memory limit.** `LimitNOFILE=65536` is already set; add a `MemoryMax=`
  only if you run several instances on one host.
- **No hardening.** `ProtectSystem`, `ReadWritePaths=` and friends are safe to
  add because the process writes only under its working directory.

## After a restart

`Restart=on-failure` re-runs the whole startup sequence, including the gate, on
a fresh connection. That is the correct behaviour: an armed engine is tied to
the connection that passed the checks, so a restart cannot reuse a stale
verdict. A clean shutdown (ctrl-c, a stream that ended normally) does *not*
restart — which only works because `RunLive` returns non-zero on a stream
abort. Without that, every dropped socket would look like a deliberate stop
and the unit would stay down.

## Paper as a daemon

`--paper` needs the same template with a different `ExecStart`. Use an override
rather than editing the shipped file, so the live unit keeps its meaning:

```ini
# sudo systemctl edit mokuren@btc_scalp_paper
[Service]
ExecStart=
ExecStart=/path/to/build/mokuren --paper --config=/path/to/configs/btc_scalp.json
```

Two instances of the same config both trade. Use distinct configs or distinct
symbols; nothing in the engine prevents a duplicate.

## Logs

The unit sends stdout and stderr to the journal, and the process additionally
writes `logs/engine.log` (rotation handled by spdlog) plus `live_debug.log` with
`--debug`. Live mode logs to the console too, so the same lines appear in both
places — expected, not a duplicate process.

`logs/live_debug_*.log` is one file per `--debug` run and is never pruned —
it's the only unbounded file in the tree. `logs/engine.log` rotates.

## Telegram notifications

`mokuren-telegram` forwards *anything* published to `mokuren:events` on
`127.0.0.1:6379`. It does not know which process published: a manual
`./mokuren --live` in a terminal and a `mokuren@…` daemon instance both reach
the same chat, interleaved. Only `TRADE_OPEN` / `POSITION_CLOSED` / `PROP_HALT`
carry `symbol`, so in practice you tell them apart by instrument, not by run.
Shadow runs (`--paper`) publish too.

- `SYSTEM_STARTUP` on `Start()`
- `SYSTEM_SHUTDOWN` on `Stop()`
- `TRADE_OPEN` on a filled entry
- `POSITION_CLOSED` on a full exit (SL / TP / trailing / scale-out complete /
  end-of-run flatten / prop-guard flatten)
- `PROP_HALT` once, on the transition into a breach
- `PROP_HALT` and any event published while the listener is restarting are
  lost: pub/sub has no replay. Treat startup/shutdown notifications as
  best-effort, not audit.
- `PublishTicker()` writes only to `mokuren:ticker` (Grafana). It does **not**
  hit the Telegram channel.

`--debug` is irrelevant to this path; it only changes the level of
`logs/engine.log` and turns on `logs/live_debug_*.log`.

## Troubleshooting

**`mokuren-telegram` is "restarting" with a growing counter.** Almost always
one of:

1. `ModuleNotFoundError: No module named 'redis'` (or `requests`, `dotenv`) at
   line 21 of `tools/TELEGRAM.py`. `ExecStart=/usr/bin/python3` is the system
   interpreter — packages installed with `pip --user` or into an activated venv
   are invisible to it. Fix:
   `sudo pacman -S python-redis python-requests python-dotenv`.
2. Missing `TELEGRAM_BOT_TOKEN` / `TELEGRAM_CHAT_ID`: the script exits 1 with
   an explicit `error:` line. `journalctl -u mokuren-telegram -n 5` shows it.

After either fix:

```bash
sudo systemctl reset-failed mokuren-telegram
sudo systemctl restart mokuren-telegram
journalctl -u mokuren-telegram -n 5 --no-pager
redis-cli PUBSUB NUMSUB mokuren:events
```

The reset is required because `StartLimitBurst=5` has already parked the unit
in `failed`.

**Engine trades but no Telegram message arrives.** Check, in order:

1. `redis-cli PUBSUB NUMSUB mokuren:events` — is the listener actually
   subscribed? If it's `0`, look at the journal for the listener.
2. `journalctl -u mokuren-telegram -f` while you send a test message — the
   `[telegram] send failed: HTTP 403` signature means the bot has never been
   `/start`-ed for that chat id.
3. `journalctl -u mokuren@… -f` for `SYSTEM_SHUTDOWN` — if the engine emitted
   it but Telegram never showed it, the listener was down (pub/sub replay is
   nil; that event is gone, not delayed).

**`200/CHDIR` on start of `mokuren@…`.** `WorkingDirectory=%h/src/kaito`
resolves to a directory that doesn't exist. Either the repo isn't at
`~/src/kaito` or `User=`/`Group=` are wrong.

## Still manual

These cannot be scripted from the repo:

- **Grafana datasource** — `localhost:3000`, `admin`/`admin`, then add the
  Redis datasource pointing at `localhost:6379` by hand.
- **Secrets** — bot token, chat id, venue keys. Never in git.
- **Per-instance config** — one JSON in `configs/` for each `mokuren@…`
  instance. The instance name (`mokuren@btc_scalp_v1`) *is* the filename.
