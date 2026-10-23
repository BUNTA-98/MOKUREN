# Running as a daemon

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
| `WorkingDirectory` | `/home/bunta/src/kaito` |
| `ExecStart` | `/home/bunta/src/kaito/build/mokuren --live --config=…/configs/%i.json` |
| `Restart` | `always`, after 5 s |
| `StandardOutput` / `StandardError` | `journal` |
| ordering | after and requiring `network-online.target` |

**Edit the paths and the user before using it.** The shipped values point at a
directory (`kaito`) that is not this project and at one specific account. An
unedited copy fails with `200/CHDIR` and then, with `Restart=always`, keeps
failing every five seconds.

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

- **No restart limiting.** `Restart=always` with a broken config or a
  permanently failing gate is an endless loop. Cap it with a drop-in:

  ```ini
  # sudo systemctl edit mokuren@btc_scalp
  [Unit]
  StartLimitIntervalSec=300
  StartLimitBurst=5
  ```

- **No timezone pin.** `TimeOfDayFilter` converts bar timestamps with the host's
  local timezone, so a `TimeOfDay` window means different hours on different
  machines. Pin it if you rely on one:

  ```ini
  [Service]
  Environment=TZ=UTC
  ```

- **No file-descriptor or memory limits.** Both websockets plus a recorder-sized
  book window fit easily in the defaults, but set `LimitNOFILE=` if you run
  several instances.
- **No hardening.** `ProtectSystem`, `ReadWritePaths=` and friends are safe to
  add because the process writes only under its working directory.

## After a restart

`Restart=always` re-runs the whole startup sequence, including the gate, on a
fresh connection. That is the correct behaviour: an armed engine is tied to the
connection that passed the checks, so a restart cannot reuse a stale verdict.

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
