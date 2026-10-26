# The TUI

`./build/mokuren` with no arguments opens the interface. It is the only mode
that logs little: startup and fatal errors go to `logs/engine.log`, everything
else is drawn.

## Pages

| key | page | shows |
| --- | ---- | ----- |
| `1` | CONFIG | the JSON config matrix, editable |
| `2` | SCANNER | grid / walk-forward runs and progress |
| `3` | EQUITY | equity curve of the last replay |
| `4` | TRADELOG | trades of the last replay |
| `5` | FOOTPRINT | candles and footprint of the last replay |

`TAB` cycles pages, `Q` quits. On the CONFIG page, `S` or `W` jumps straight to
the scanner and starts the sweep without a second keypress.

A finished scan opens nothing by itself: the leaderboard is filled and the
status line returns to `IDLE`. Select a row with `UP`/`DOWN` and press `ENTER`
to replay it; that replay switches to the trade log and becomes the data the
equity and footprint pages draw from.

## CONFIG: editing a profile

The editor opens `configs/default.json`. Leaving the page and coming back keeps
the profile you picked during the session; nothing is remembered across
restarts.

A file that is missing, unreadable, or not valid JSON is silently replaced with
the built-in default, and that default is written to disk. Keep a copy of a
profile you care about — a broken comma in it is not reported, it is erased.

Three columns, left to right: top-level section, sub-section (a component's
`name`, or `General` for scalar settings), parameter. Arrows move between them;
`ENTER` opens the highlighted parameter.

| value type | `ENTER` does |
| ---------- | ------------ |
| bool | toggles and saves immediately |
| number | opens the value configurator (`STATIC` / `RANGE`) |
| string | opens an inline editor; `ENTER` saves, `BACKSPACE` deletes |
| `dataset` | cycles the sub-directories of `data/` |
| `config_profile` | cycles the files, `.csv` and `.json`, under `configs/` |

Every number opens the configurator, not just an existing grid node. `STATIC`
edits a single `Val:`; `RANGE` edits min / max / step and writes the node as
`{"mode":"range",…}` so the next sweep picks it up. `[ SAVE ]` applies and
reloads the file, `[ CANCEL ]` discards. A scalar shown as
`[GRID] min : max : step` is the same editor already in range mode.

Choosing a different `config_profile` writes the current file, then loads the
chosen one and rebuilds the menu from it. Every other save also rewrites the
file and reloads, so the menu always shows what is on disk.

That row exists only when the profile carries a `config_profile` key; the editor
adds one automatically inside a `backtest` block. Without it there is no way to
switch files from inside the editor.

A component row is dimmed while its `active` / `enabled` flag is false; the
flag itself is an ordinary row in that component's column.

## SCANNER

`S` and `W` start a grid search or a walk-forward sweep. Both are ignored while
one is running: a scan is not interrupted, and there is no cancel key. Either
key needs the profile to name a dataset; without one the status line reads
`ERR: no dataset in <path>` and nothing runs.

Progress is per run. The status line names what is being evaluated and the
progress bar counts completed runs; a sweep with no grid node completes as a
single run. Results stream into the leaderboard as they arrive and stay sorted
by net profit, keeping the top 100.

### Leaderboard keys

| key | effect |
| --- | ------ |
| `UP` / `DOWN` | select a run |
| `D` | overlay the selected run's parameter set |
| `ENTER` | replay the selected run |
| `H` | 2D heatmap of the leaderboard |

### Heatmap

The two parameters with the most distinct values across the leaderboard become
the axes (the one with more distinct values on X); each cell is coloured by
net profit. A parameter that never varied has one value and is not a
candidate. `ARROWS` move
the cursor, `ENTER` replays that cell, `H` or `ESC` closes. A cell with no run
behind it is drawn hollow, and the bottom lines show the selected cell's pnl,
win rate, drawdown and trade count.

## Reading results

### TRADELOG

`UP`/`DOWN` select a trade, `D` opens its stop-loss timeline — every break-even
and trailing move with a timestamp — and any key closes that overlay. `ENTER`
switches to the footprint of that trade; the target candle is not selected yet.

### EQUITY

Braille curve of cumulative net profit, plus trade count, net pnl and max
drawdown. The drawdown is computed against a fixed 10,000 starting balance, so
a run funded differently (a `--paper` config with its own `initial_balance`)
shows a percentage that does not match the engine's own figure.

### FOOTPRINT

`LEFT`/`RIGHT` step one candle, `ENTER` toggles between the price chart and the
footprint ladder, `T` switches 1-minute and 15-minute, `V` switches the ladder
between base and quote volume, and `n`/`N` jump to the next/previous trade
entry.

The ladder marks imbalance at ratio 1.5 — the `StackedImbalance` default, not a
config value — highlights the POC, and tags the entry, the exit and every stop
level of the trade active at that candle.

## Boundaries

- A grid node saved through the editor is written as
  `{"mode":"range","min","max","step","val"}` with `val` set to `min`. Saving a
  range therefore changes the value a live run would use.
- The `dataset` selector lists directories only. A `.mkn` recording must be
  typed into the config file or reached through a profile that already names
  one.
- The first entry of both selectors is the folder name itself — `data` for the
  dataset row, `configs` for the profile row. Neither is a valid choice:
  selecting `configs` leaves the editor unable to open its file, and every save
  afterwards is lost.
- There is no undo and no confirmation. Toggling a bool rewrites the profile
  immediately.
- The scanner refuses to start when no profile has been selected, rather than
  silently loading a default.
