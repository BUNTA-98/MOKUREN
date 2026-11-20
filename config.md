# Config system: schema in code, profiles as data

Status: P1, P1b, §13, §15.9 and §15.10 done -- `config_schema.{hpp,cpp}` landed,
`AppConfig::Load` reads through the table, `DataManager` resolves the bare
`data/` root to the newest dataset beneath it, and every profile in `configs/`
either loads unedited or was fixed to (now a build gate, see
`test/test_profiles.cpp`). Open: P2. Sections 15--19 hold the P1b execution list
and the TUI layout spec; 1--14 are the design they implement.
Scope: `AppConfig` / `config_parser` / the config page of the backtest TUI.

---

## 1. Why

Today the JSON file *is* the schema. `PageConfig::FlattenJson` walks whatever keys
exist and builds the menu from them; `AppConfig::Load` reads the same keys with a
hardcoded fallback per field (`SafeNumber(env, "interval_ms", 60000)`).

Two consequences, both real:

- **A new feature is invisible until every profile is hand-edited.** Not "off" --
  absent. The UI cannot show a row for a key that no file mentions, so there is no
  way to turn it on from the TUI, and the toggle path
  (`config_data[row.ptr] = !config_data[row.ptr].get<bool>()`) throws if it is
  ever reached. `configs/_defaults.json` exists only to seed new profiles; it is
  not consulted for existing ones.
- **Defaults have three owners** (struct initializer, `Load`'s fallback argument,
  `_defaults.json`) and nothing checks that they agree. `max_cores` is already
  broken this way: `"max_cores": 6.0` in `configs/default.json` is silently
  ignored because `Load` tests `is_number_integer()`, which is false for a float.

## 2. The invariant

> One compiled table says which fields exist, what they mean, and what their
> defaults are. Profiles hold values. Nothing else knows field names.

Definition of done: **adding a field to the table makes it appear in the UI and in
every run with its default, without a single config file being edited.**

## 3. Decisions

**D1 -- Schema is one compiled table, one spelling per field.**
`include/config_schema.hpp` + `src/config_schema.cpp`. It replaces
`configs/_defaults.json`, the per-field fallbacks in `Load`, and every aliasing
chain in the loader.

**No aliases are kept.** Profiles are rebuilt against the new table, so
backward compatibility is explicitly not a goal. Accepted spellings are removed
rather than deprecated; a profile still carrying one fails with
`config error: unknown key '<path>'` (plus a nearest-match hint). The full list
is in 10.13. This removes `std::initializer_list<const char*> aliases` from the
descriptor entirely -- there is one key per field, so the reader has nothing to
try twice.

**D2 -- `Load` merges, it does not invent.** Walk the schema, take the file's value
if present and valid, else the table default. The reader helper **never takes a
fallback argument** -- that is the rule that kills the current soup.

**D3 -- Profiles are overlays.** A profile may contain only what differs from
defaults. New fields need no migration. A profile that still uses a removed
spelling (10.13) fails at load instead of loading with a different meaning.

**D4 -- Do not materialize defaults into the in-memory JSON.** The UI's
`config_data` stays exactly what the file said, plus whatever the user edited.
Menu rendering and toggling resolve through the table. This is not a style
choice -- see 10.1 and 10.2; materializing silently re-arms guards.

**D5 -- One grid-node predicate.** `has numeric min/max/step` + `scalar numeric
val` + the field is marked `Sweepable`. All three current, disagreeing predicates
call it.

**D6 -- Profile selection is UI state, not a config key.** `backtest.config_profile`
is deleted as a config key; the last-used profile lives in a small session file
next to the binary's state. Kills the unconditional self-write in
`LoadConfigSmart` and the "which file is merged?" ambiguity.

`ui_menu.hpp` shows this key is also a menu row with a special-case
`INLINE_EDIT` handler that saves the current file, swaps `config_path`, and
reloads. That handler goes away with the key: the profile picker becomes a menu
action backed by `SetActiveConfigPath`, not a row that happens to contain a path.
P2 changes both (`BuildMenu` and the picker action); leaving one of them and not
the other is how the menu ends up with a `backtest > config_profile` row that
writes to a key nothing reads.

**D7 -- The `Required` set is what the loader already refuses to default,
plus `initial_balance`.** Not a new policy. A field is `kRequired` if
`AppConfig::Load` (or a resolver it calls) already throws when it is absent.
Measured against `config_parser.cpp` and `trade_management.hpp`, that set is:

- `exchange`
- `environment.interval_ms`, `environment.macro_interval_ms`,
  `environment.tick_size`, `environment.vwap_reset_hour`
- `execution.taker_fee_pct`, `execution.slippage_pct`
- `risk.sl_pct`, `risk.tp_r`, `risk.risk_per_trade_pct`
- `trade_management.break_even.{trigger_r,target_r}`
- `trade_management.trailing.{enabled,trigger_r,distance_r}`
- `trade_management.scale_out.{enabled,trigger_r,fraction}`
- `initial_balance` (promoted; see below)

Everything else keeps the default it has today in `SafeNumber`/`SafeBool`/
`SafeString`/`PropRules`/`ProbeCriteria`. In particular `venue.environment`,
`venue.category`, `venue.symbol`, `venue.position_mode`, `venue.lot_step`,
`venue.allow_mainnet` stay optional with their current defaults, and
`environment.dataset` is the one field whose *new* default (`"data/"`) is still
pending a small `DataManager` change (section 14). P1b is then a relocation of
the existing policy, not a behavior change -- the config's meaning is what it
was, minus the aliases 10.13 removes.

`initial_balance` is promoted to required. It is the denominator of every
drawdown figure and the capital `PaperTrader` is seeded with; there is no
defensible default for "how much money is this account", and a compiled-in 10000
armed the prop guard against capital the config never named (10.1b). Making it
required deletes the conditional check, keeps `prop.initial_balance` refused for
good, and leaves exactly one owner for the number.

The list is long because the current loader is strict, not because the new one
invents strictness. `configs/default.json` carries every field above and
continues to load unchanged. The visible change is the *other* direction: a new
optional field appears in the TUI with its table default instead of being
invisible, which is the invariant (section 2).

## 4. The field descriptor

```cpp
namespace mokuren::cfg {

enum class Kind : uint8_t {
  Bool,     // toggled with ENTER
  Int,      // stepper
  Double,   // stepper
  String,   // inline edit
  Enum,     // left/right over | separated values
  DirPath,  // selector over ScanDirectory(root, only_dirs=true)
  FilePath, // selector over ScanDirectory(root, only_dirs=false)
};

enum : uint32_t {
  kRequired   = 1u << 0,  // absent -> error, not default (D7)
  kSweepable  = 1u << 1,  // may hold a grid node instead of a scalar
};
// There is no kLegacy. A removed spelling is an error, not a tolerated input
// (D1, 10.13). Nothing is written that the table does not name.

struct Field {
  const char* path;        // "risk.sl_pct"          -- dotted, no leading slash
  Kind        kind;
  json        default_value;
  const char* category;    // UI column 1, e.g. "risk"
  const char* subcategory; // UI column 2, "General" when unset
  const char* label;       // UI column 3
  int         order;       // sort key within (category, subcategory)
  uint32_t    flags = 0;
  double      min = 0.0;   // inclusive domain for Int/Double; min==max==0 => unbounded
  double      max = 0.0;
  const char* enum_values = nullptr;  // "one_way|hedge"
  const char* selector_root = nullptr; // "data" / "configs" for Dir/FilePath
};

const std::vector<Field>& Schema();      // built once, immutable after init
const Field* Find(std::string_view path); // nullptr when the path is unknown

} // namespace mokuren::cfg
```

Notes:

- The table is a function-local `static const std::vector<Field>` built on first
  call. It is **not** `constexpr` -- `json` members prevent that -- but it is
  effectively immutable and costs one initialization at startup. Do not reach for
  X-macros to get constexpr; the readability loss is not worth it at this size.
- **Coercions are preserved; aliases are not.** `GetDouble`/`GetInt` accept
  numeric strings (`"0.004"`) and whole-valued floats (`6.0` as `6`); `GetBool`
  accepts `true`/`"true"`/`"1"`/`1`. Hand-written profiles rely on this today
  (`environment.interval_ms: 60000.0`, `max_cores: 6.0`) and none of it changes
  which key is read. `risk.sl_price_pct` is a different *path*, so it is
  rejected outright. A value's spelling is tolerated; a key's name is not.
- A default that cannot be a literal is a sentinel the loader resolves, not a
  hook on `Field`. `environment.max_cores` defaults to `0 == auto`
  (`hardware_concurrency()`), matching the convention `AppConfig` already uses.
  Do not add a `default_fn` to the descriptor for one field.
- `Subcategory` defaults to `"General"` so the existing
  three-panel layout keeps working unchanged.
- One key per field. There is no alias list: an unrecognised spelling is rejected
  at load with the dotted path and the nearest known key (10.13).
- **`min`/`max` are advisory; there is no `kRange` flag.** Inclusive
  `[min,max]` cannot express `(0, 0.1)` or `> 0`, and `Validate()` already
  enforces every numeric bound (including derived values such as
  `tp_pct = tp_r * sl_pct`) with a precise message. The descriptor keeps
  `min`/`max` only so the TUI stepper knows its bounds. Validity has exactly one
  enforcement point, in `AppConfig::Validate()` -- two would be the same
  two-owners bug this project removes.

### Reader API (the part `Load` uses)

```cpp
namespace mokuren::cfg {

// Reads src[path], validated against the table. Falls back to the field default.
// Throws when a Required field is absent, when the type does not match, when the
// value is outside [min, max], or when an Enum value is not in enum_values.
// Enum is read with GetString; a separate getter would only restate the check.
double      GetDouble(const json& src, std::string_view path);
int         GetInt(const json& src, std::string_view path);
bool        GetBool(const json& src, std::string_view path);
std::string GetString(const json& src, std::string_view path);

// "did the author write it?" -- ignores defaults. The ONLY correct way to test
// presence. One caller (prop.enabled); keep it that way. See 10.1.
bool Has(const json& src, std::string_view path);

} // namespace mokuren::cfg
```

There is deliberately no `GetDouble(src, path, fallback)`. If a caller wants a
different default, the table is wrong -- fix the table.

## 5. What the table covers

Covered (scalar sections):

`initial_balance`, `api_key`, `api_secret`, `api_passphrase`,
`environment.*`, `venue.*` (including `data_environment`, `data_exchange`;
`is_testnet` is derived from `venue.environment` and is not a config key),
`execution.*`, `risk.*` (top-level scalars), `trade_management.*` (all three
sub-blocks), `prop.*`, `startup_probe.*`.

Not covered in v1 -- **file-driven, as today**:

- **Component arrays** (`triggers[]`, `filters[]`, `risk.modules[]`). Each entry
  is `{name, ...component-specific params}`, and the UI derives the subcategory
  from `name`. A flat table cannot enumerate these. Per-component schema is a
  separate project; if it happens, it extends this table rather than replacing it.
- **`backtest.*`** (`wfa_*`, `config_profile`). Nothing in `AppConfig::Load`
  reads these, so a second reader already exists outside the config layer.
  Out of scope here; `config_profile` is removed by D6 regardless.
- **`trade_management.structural_exit.rules[]`.** The block's scalars
  (`enabled`, `min_holdtime_ms`) are in scope. The `rules` array has the same
  `{name, ...params}` shape as `triggers`, and its per-rule
  `enabled`/`active`/`isActive` spellings travel with it (all three are honoured
  by `ResolveStructuralExitConfig::read_enabled`). `BuildExitPlan` interprets
  each name's params: `MfeGiveback{giveback_r}`,
  `TimeStop{max_hold_ms}`, `ImbalanceBreak{ratio,min_levels}`,
  `DeltaFlip{min_delta}`, `SpoofPull{drop_pct,min_qty}`,
  `DeltaDivergence{lookback}`. Unchanged in v1, same treatment as components
  above -- which is *not* the same as frozen: `configs/backtest_structural_exit.json`
  sweeps `ImbalanceBreak.min_levels` and `ImbalanceBreak.ratio`, so a rules[]
  param is swept through the component path, exactly like a trigger param.

### 5.1 Field inventory (code-derived)

`configs/default.json` is the most complete profile in the repo, and it is still
not the authority. `AppConfig::Load` in `src/config_parser.cpp`, plus the
resolvers it calls (`trade_management.hpp`, `prop_guard.hpp`, `startup_gate.hpp`),
is the source: a scalar exists because a reader reads it, not because some
profile happens to carry it. Deriving the list from a config would reproduce the
exact bug this plan fixes -- a key the code reads but no file mentions is
invisible, which is indistinguishable from a key the code does not read.

`configs/default.json`, `configs/default.json.bak` and `configs/_defaults.json`
are used only as a **cross-check** that no reader was missed. When they disagree
with the code, the code wins; a key in one of them that is absent below is the
finding.

Status: **R** = required today (absent is an error), **O** = optional with a
fallback, **X** = removed by 10.13, **?** = field exists on the struct but the
default is unconfirmed (needs the file in 5.2).

**Implemented** in `include/config_schema.hpp` + `src/config_schema.cpp`, and
consumed by `AppConfig::Load` since P1b. The table's `order` values also carry
category ordering: each section starts at a distinct base (100 Account,
200 Environment, 300 Venue, 400 Execution, 500 Risk, 600 Trade Management,
700 Prop, 800 Startup Probe). Defaults are a consolidation of the loader
fallbacks and `configs/_defaults.json`, made consistent -- sensible, not a
line-for-line port of any one file. The `startup_probe.*` numbers are
`ProbeCriteria::Development()`'s, transcribed in P1b (15.6a); a dedicated test
compares them against the header, because the §13.3 round trip compares two
loads of the same table and cannot see a transcription error.

Root:

| path | kind | st | note |
|---|---|---|---|
| `exchange` | Enum(bybit/hyperliquid/blofin) | R | also `environment.exchange`, object form (X) |
| `initial_balance` | Double | R | promoted by D7; today O + conditional prop check |
| `api_key` | String | O | root wins; env-var fallback |
| `api_secret` | String | O | |
| `api_passphrase` | String | O | required only when exchange=blofin and live |
| `category` | Enum(linear/spot/inverse) | X | root override of `venue.category` |
| `symbol` | String | X | root override of `venue.symbol` |
| `allow_mainnet` | Bool | X | root override of `venue.allow_mainnet` |
| `is_testnet` | Bool | X | now `venue.environment` |
| `data_exchange` | String | X | now `venue.data_exchange` |
| `data_venue` | Object | X | now `venue.data_*` |

`environment`:

| path | kind | st | note |
|---|---|---|---|
| `dataset` | DirPath(root=`data`) | O | default `"data/"`, resolved by `DataManager` to the newest dataset beneath it (15.9) |
| `interval_ms` | Int | R | > 0 |
| `macro_interval_ms` | Int | R | >= `interval_ms` |
| `tick_size` | Double | R | > 0 |
| `vwap_reset_hour` | Int | R | [0,23] |
| `max_cores` | Int | O | 0 = auto; `6.0` silently ignored today (10.8) |
| `filepath` | String | X | legacy alias of `dataset` |

`venue`:

| path | kind | st | note |
|---|---|---|---|
| `environment` | Enum(mainnet/testnet/demo) | O | default testnet; `env` is X |
| `category` | Enum(linear/spot/inverse) | O | default `linear` |
| `symbol` | String | O | default `BTCUSDT` |
| `position_mode` | Enum(one_way/hedge) | O | default `one_way` |
| `lot_step` | Double | O | > 0 |
| `allow_mainnet` | Bool | O | must be true when env=mainnet and exchange=bybit |
| `data_environment` | Enum(mainnet/testnet/demo) | O | default = `environment` |
| `data_exchange` | Enum(bybit/hyperliquid/blofin) | O | default = `exchange` |
| `env`, `data_venue` | | X | |

`execution`:

| path | kind | st | note |
|---|---|---|---|
| `taker_fee_pct` | Double | R | [0, 0.1) |
| `slippage_pct` | Double | R | [0, 0.1) |

`risk`:

| path | kind | st | note |
|---|---|---|---|
| `sl_pct` | Double | R + kSweepable | (0,0.1); defines 1R |
| `tp_r` | Double | R + kSweepable | (0,100]; grid node in `default.json.bak` |
| `risk_per_trade_pct` | Double | R | (0,0.1) |
| `min_distance_dollars` | Double | O | 0 = never widen |
| `max_daily_loss` | | X | ignored today; rejected |
| `sl_price_pct`, `tp_pct` | | X | |
| `modules[]` | array | OOS | section 5 |

`trade_management`:

| path | kind | st | note |
|---|---|---|---|
| `break_even.enabled` | Bool | O | default **true** -- the only enabled-by-default block |
| `break_even.trigger_r` | Double | R | |
| `break_even.target_r` | Double | R | |
| `trailing.enabled` | Bool | R | `RequireBool`, unlike break_even |
| `trailing.trigger_r` | Double | R | |
| `trailing.distance_r` | Double | R | < `trigger_r` when enabled |
| `scale_out.enabled` | Bool | R | `RequireBool` |
| `scale_out.trigger_r` | Double | R | |
| `scale_out.fraction` | Double | R | [0.1,0.9] |
| `structural_exit.enabled` | Bool | O | default **false** -- opposite of `prop.enabled`, and safe: absence or `false` is a no-op, `ResolveStructuralExitConfig` returns before validating anything |
| `structural_exit.min_holdtime_ms` | Int | R* | required only when `enabled`, > 0; validated by the resolver, not expressible as `kRequired` |
| `structural_exit.rules[]` | array | OOS | section 5; at least one non-disabled rule required when `enabled` (resolver check, same class as `min_holdtime_ms`) |
| `*_pct` forms | | X | derived from the `_r` forms |

`prop`:

| path | kind | st | note |
|---|---|---|---|
| `enabled` | Bool | R* | presence is the switch today; D7 makes it explicit |
| `max_daily_drawdown_pct` | Double | R* | when enabled, (0,1] |
| `max_total_drawdown_pct` | Double | R* | when enabled, (0,1] |
| `warn_fraction` | Double | R* | when enabled, (0,1) |
| `flatten_on_breach` | Bool | O | `PropRules::flatten_on_breach` = true |
| `daily_reset_hour_utc` | Int | O | `PropRules::daily_reset_hour_utc` = 0, [0,23] |
| `initial_balance` | | X | rejected explicitly |

`startup_probe`:

| path | kind | st | note |
|---|---|---|---|
| `preset` | Enum(development/production) | O | seeds the rest |
| `min_trades_per_min` | Int | O | `Development()` = 60 |
| `min_l2_frames_per_min` | Int | O | `Development()` = 120 |
| `min_duration_ms` | Int | O | `Development()` = 5000 |
| `timeout_ms` | Int | O | `Development()` = 30000, >= `min_duration_ms` |
| `max_reconnects` | Int | O | `Development()` = 0 |
| `min_l2_depth` | Int | O | `Development()` = 2, <= `MAX_L2_DEPTH` |
| `on_failure` | Enum(abort/warn) | O | sets `probe_abort_on_failure` |

Three things the inventory already proves about the plan:

1. **`trade_management`'s four sub-blocks have four different enablement
   policies.** `break_even.enabled` is optional and defaults **true**;
   `trailing.enabled` and `scale_out.enabled` are required; and
   `structural_exit.enabled` is optional and defaults **false**, the opposite of
   break_even. The table must carry the per-field `kRequired` flag *and* the
   per-field default exactly as today, or a profile that omits one flips a block
   on or off for the wrong reason. This is the strongest argument for a per-field
   flag over any per-section rule.
2. **`structural_exit.min_holdtime_ms` is required conditionally on
   `structural_exit.enabled`.** `kRequired` cannot express that, so `Load` keeps
   one cross-field check after the table read. Write it down or someone will
   "simplify" it away; the same shape applies to the `prop` block's four `R*`
   rows.
3. **Every former `?` row took its fallback from `PropRules`/`ProbeCriteria`,
   so the table default had to be transcribed from those headers and not from
   `default.json`** -- which carries `min_l2_depth` and `on_failure` but omits
   the other five, and would therefore have "confirmed" two numbers out of
   eight. Done in P1b: the `prop.*` defaults match `PropRules{}` and the
   `startup_probe.*` defaults match `ProbeCriteria::Development()`, and a test
   compares the table against the headers rather than trusting the
   transcription.
4. **`risk.max_daily_loss` is silently ignored today**, not rejected:
   `config_parser.cpp` says so in a comment. Under D1 it becomes an unknown-key
   error. That is a behaviour change on any profile still carrying it --
   `configs/default.json.bak` does -- so it is a one-line edit for the operator,
   but name it in the release note rather than letting it surface as "my config
   stopped loading".

### 5.2 What is still unconfirmed

Resolved by the two greps:

- **There is no `exit_strategy` block.** The feature is
  `trade_management.structural_exit`, parsed by
  `mokuren::ResolveStructuralExitConfig` inside `AppConfig::Load`. It was always
  in scope (section 5).
- **`StructuralExitConfig` has one consumer chain**:
  `ResolveStructuralExitConfig` -> `BuildExitPlan` -> `StructuralExitGuard`.
  `GetPositionEntryTime` is declared once (`ibroker.hpp:77`, default 0),
  overridden only by `PaperTrader` (`papertrader.hpp:65`), and read only from
  `structural_exit.hpp:568`. So the table's job for
  `structural_exit.min_holdtime_ms` really is "one resolver", as section 5 says.
- **`backtest.*` has one reader outside the config layer**: `src/wfa_runner.cpp`.
  It is the only file the grep matched. Q5 is settled -- moving WFA settings into
  the schema means touching that file, which is why it stays out of v1.

Still needed, in priority order:

1. **`configs/_defaults.json`** -- read, and it loads unedited. Its only grid
   node outside a component array is `risk.sl_pct`, which is `kSweepable`, so it
   needs no edit; it is still the file `LoadDefaults()` reads until P3.
2. `include/prop_guard.hpp` -- read. `PropRules{}` is
   `{enabled=false, 0.04, 0.06, 0.8, daily_reset_hour_utc=0,
   flatten_on_breach=true}`, and it deliberately carries no starting balance,
   which is why `prop.initial_balance` stays refused rather than defaulted. Both
   former `?` rows are resolved.
3. `include/startup_gate.hpp` -- read. `Development()` is
   `{60, 120, min_l2_depth=2, 5000, 30000, 0}` and `Production()` differs in
   four of those six (l2 rate, duration, timeout, reconnects), which is what
   makes the `preset` cascade (15.6a) worth having at all. All six former `?`
   rows are resolved.
4. `include/venue_spec.hpp` -- exact `Env` spellings `ParseEnv` accepts, and
   `IsKnownCategory`'s list. `enum_values` must be the loader's list, not a
   paraphrase of it.
5. `include/factory.hpp` + `src/factory.cpp` -- confirm the factory reads no
   top-level scalar beyond `AppConfig`, and see where `BuildExitPlan` is called.
6. `src/data_manager.cpp` -- confirm `LoadAll("data/")` on a bare root returns
   empty today, so section 14's test fails before the fix.
7. `include/ui_pages.hpp`, `include/ui_manager.hpp`, `src/ui_manager.cpp` -- P2
   only. Now more urgent: `ui_menu.hpp` shows `config_profile` is *also* an
   `INLINE_EDIT` special case that switches profiles from inside the menu, so D6
   changes that path too (see 11).

## 6. Resolution order

```
schema default
  <- value in the profile file (a scalar, or a grid node if kSweepable)
    <- nothing else
```

CLI flags are not config. `--config`, `--live`, `--paper` select *which* run and
*which* file; they do not override values. If a real need for overrides appears,
add a layer above the profile -- but do not add one speculatively.

`FlattenGrids(json&)` stays where it is (top of `Load`), because the grid path
generates scalar permutations before calling `Load`. The predicate moves into
`cfg::IsGridNode`; `FlattenGrids` calls it.

## 7. Profiles

- **Save = the in-memory json.** With D4, that object is already minimal: the
  keys the file had, plus the keys the user edited. No diffing machinery needed.
- **Never write without a keystroke.** Today `LoadConfigSmart` saves when the file
  is missing or does not parse, and rewrites `backtest.config_profile` on every
  load. Both go away; loading is read-only.
- **"Export full profile"** (P4, optional) writes the table defaults merged with
  the current values, for archiving a run's exact configuration. Separates
  "working file" from "record of a run".
- The consequence of overlay-only profiles: a value deliberately pinned at
  today's default will follow a future default change. That is correct for a
  tuning file and wrong for a record -- hence the export path.

## 8. The UI: how the menu is built

The renderer is unchanged (three panels, same key handling). What changes is
where its contents come from. Instead of discovering rows by walking the file,
`PageConfig::BuildMenu()` assembles them from three explicit sources.

### 8.1 Three sources

1. **Schema rows.** Every `cfg::Field` in `Schema()` becomes one row, filed under
   `(field.category, field.subcategory)`. A field's dotted `path` is where the
   value lives; `category`/`subcategory`/`label` are where it renders. They are
   not required to be the same string -- `venue.environment` can render under
   `VENUE > General > environment` while storage stays dotted.

2. **Component arrays**, discovered from the file. These cannot be in the table
   (section 5), and the list of them is part of the design, not an accident of
   naming:
   ```cpp
   // Paths the schema deliberately does not model. Each is an array of
   // {name, ...params}; each item's `name` becomes a subcategory, each param
   // in the item becomes a row -- the existing FlattenJson logic, scoped to
   // one subtree.
   constexpr std::string_view kComponentArrayPaths[] = {
     "triggers",
     "filters",
     "risk.modules",
     "trade_management.structural_exit.rules",
   };
   ```

3. **File-driven subtrees.** Whole sections shown in the menu but not in the
   schema. Currently only `backtest`, whose scalars are read by the WFA runner
   rather than `AppConfig::Load` -- out of v1 scope by section 5, but its
   settings must not vanish from the UI:
   ```cpp
   constexpr std::string_view kFileDrivenSubtrees[] = {
     "backtest",
   };
   ```

**Everything else is an unknown key.** A scalar path that is not in `Schema()`
and not under one of the two lists above is reported by the 10.13 lint and *not*
rendered. That is the whole point: a typo like `risk.slipage_pct` must not become
a menu row the user tunes forever while the loader silently reads
`risk.slippage_pct` beside it.

### 8.2 BuildMenu

```cpp
void PageConfig::BuildMenu() {
  categories.clear();
  subcategories.clear();
  grouped_rows.clear();

  // (1) schema rows
  for (const cfg::Field& f : cfg::Schema()) {
    MenuRow row;
    row.def = &f;
    if (f.kind == cfg::Kind::DirPath || f.kind == cfg::Kind::FilePath) {
      row.is_selector = true;
      row.options = ScanDirectory(f.selector_root,
                                  /*only_dirs=*/f.kind == cfg::Kind::DirPath);
      // NO NO_DATASETS_FOUND sentinel (10.7). an empty list is an empty list,
      // and the picker refuses to write a path when there is nothing to pick.
    }
    grouped_rows[f.category][f.subcategory].push_back(std::move(row));
  }

  // (2) component arrays, existing FlattenJson logic scoped to one subtree
  for (auto path : kComponentArrayPaths) {
    const json* node = Lookup(config_data, path);
    if (node && node->is_array()) FlattenComponents(*node, path);
  }

  // (3) file-driven subtrees
  for (auto path : kFileDrivenSubtrees) {
    const json* node = Lookup(config_data, path);
    if (node && node->is_object()) {
      FlattenJson(*node, std::string("/") + std::string(path), /*level=*/0,
                  std::string(Leaf(path)), "General");
    }
  }

  // (4) order within each bucket
  for (auto& [cat, subs] : grouped_rows) {
    for (auto& [sub, rows] : subs) {
      std::stable_sort(rows.begin(), rows.end(), [](const MenuRow& a,
                                                    const MenuRow& b) {
        if (a.def && b.def) return a.def->order < b.def->order;
        if (a.def) return true;   // schema rows before file rows
        if (b.def) return false;
        return false;             // both file rows: keep insertion order
      });
    }
  }

  // (5) category and subcategory order
  OrderCategoriesAndSubcats();
}
```

The three order questions, decided once:

- **Category order** = the smallest `Field.order` any of its fields declares.
  File-driven categories (`backtest`) own no schema field, so they take
  `INT_MAX` and sort alphabetically among themselves, last. No second list to
  maintain; a new category appears in the right place because it is in the
  schema.
- **Subcategory order** = `General` first (matching the current rotation in
  `LoadConfigSmart`), then by smallest `Field.order` within
  `(category, subcategory)`, then file-driven subcategories in array order.
- **Row order** = `Field.order` for schema rows, array position for component
  rows. Menu order is now intentional; it was
  alphabetical-by-nlohmann-`std::map` before.

### 8.3 MenuRow

```cpp
struct MenuRow {
  const cfg::Field* def = nullptr;   // null for a file-driven row
  bool is_selector = false;
  std::vector<std::string> options;  // DirPath / FilePath only
  int current_idx = 0;
};
```

The four type bools (`is_bool`/`is_number`/`is_string`/`is_range`) are derived at
render time from `def->kind` (`nullptr` => a file-driven row keeps the old
shape-derived behaviour) and `IsGridNode(value)`. `is_file_selector` becomes
`def && (def->kind == DirPath || def->kind == FilePath)` -- the table decides,
not `current_path.find("dataset")` (10.7).

### 8.4 Reading and writing

- **Display**: `value = Has(config_data, path) ? config_data[path] : def->default_value`.
  The effective value is visible; nothing is written.
- **Toggle/edit of an absent key**: write `def->default_value` (inverted or
  edited) into `config_data` first, then apply the edit. This is the fix for the
  crash in 10.3 and it is one branch.
- **Grid row**: `def->flags & cfg::kSweepable` decides whether the "make this a
  sweep" submenu is offered (§9). A grid node on a non-sweepable schema field is
  still *displayed*, as a grid, so the user can edit it back to a scalar;
  `Load` rejects it if they do not.
- **`Render` stops mutating state.** `Render`'s cursor clamping moves to
  `HandleInput` and `LoadConfigSmart`; the draw path becomes a pure function of
  state or the two panels can disagree mid-frame.
- **Column layout** stays at 2 / 21 / 43 / 45 / 47 / 75, but `label` in the table
  is capped (say 24 chars) and the value truncates to what remains. Verify with
  the longest label in the table before merging.

## 9. Grid nodes

Two questions, two predicates. Conflating them is how the current code ended up
with three disagreeing rules.

**Does this json node represent a sweep?** Shape only -- no schema lookup:

```cpp
// an object with numeric min/max/step and a scalar val. no knowledge of the
// field it sits on: a component-array param (triggers[0].min_delta) is swept
// exactly like a schema scalar (risk.tp_r).
bool IsGridNode(const json& j);
```

This is what `FlattenGrids`, `GridScanner::FindParams`, and the menu's range
display all call. It must **not** require `kSweepable`:
`configs/backtest_structural_exit.json` sweeps `triggers[1].levels`,
`trade_management.structural_exit.rules[1].min_levels`, and
`trade_management.structural_exit.rules[1].ratio` -- none of those paths has a
`Field`, and a predicate that required one would reject every component sweep in
the repo.

**Is a sweep legal on this field?** Schema lookup, at load and in the UI:

```cpp
// the field is kSweepable and the json really is a grid node.
bool IsSweepableValue(const json& j, const Field& f);
```

- `Load` rejects a grid node on a non-`kSweepable` scalar field ("`prop.warn_fraction`
  cannot be swept"), naming the path.
- The menu offers "make this a sweep" only when `f.kSweepable`, but still
  *shows* a grid node on a non-sweepable field so it can be edited back.

A `{min,max,step}` object with no `val` is not a sweep under either predicate,
and is a load error naming the missing `val` -- never a silent scalar fallback,
because "swept nothing, ran once" is indistinguishable from "worked".

Current predicates, and why they disagree:

| site | rule |
|---|---|
| `PageConfig::FlattenJson` | `mode == "range"` **or** has min/max/step |
| `GridScanner::FindParams` | min/max/step, all numeric, no `val` needed |
| `FlattenGrids::IsGridNode` | min/max/step + scalar `val` |

The `{min,max,step}`-without-`val` case is the visible disagreement: it renders
as a sweepable range in the menu, is picked up by `GridScanner`, and then fails
`RequireNumber` at load with "must be a number (got {...})". The shape predicate
makes that one error, at the UI boundary, naming the missing key.

## 10. Traps in the existing code

### 10.1 Presence vs value -- the dangerous class

Two load-time checks ask "did the author write this key?" and would silently
change meaning the moment defaults become resolvable:

**(a) `prop.enabled` is armed by the block existing.**
```cpp
cfg.prop.enabled = SafeBool(p, "enabled", true);
if (cfg.prop.enabled) {
  if (!has_initial_balance) throw ...;
```
A profile with `"prop": {}` arms the guard. Give `prop.enabled` a table default of
`false` and that profile silently stops enforcing anything.

**(b) `initial_balance` presence gates the prop base.**
```cpp
const bool has_initial_balance = src.contains("initial_balance");
...
if (!has_initial_balance) throw "a config with a 'prop' block needs an explicit 'initial_balance'";
```
This exists because falling back to the compiled-in 10000 armed the guard against
an account nobody described. Once the table gives `initial_balance` a default,
`has_initial_balance` is always true -- the check never fires -- and a
`prop`-enabled profile without a root `initial_balance` arms at 10000 again.
**Silently.** D7 removes the hazard at the root instead of guarding it:
`initial_balance` is required, so there is no default to fall back to,
`has_initial_balance` disappears from `Load`, and this whole branch goes with it.

Mitigation:

- **(b) is deleted by D7, not patched.** Nothing to test at load; absence is
  already an error, naming `initial_balance`.
- **(a) is still a real toggle.** `prop.enabled` defaults to `false`, and a
  profile carrying a `prop` block without an explicit `enabled` is a **load
  error** naming what to add. Falling back to `false` would silently disarm a
  profile whose author wrote the block to arm it -- worse than a failed start.
- Presence goes through `cfg::Has(src, path)` on the *source* json, never a
  resolved value. That rule now has exactly one caller, and saying so at the top
  of `config_schema.hpp` is what stops it growing a second meaning.
- `prop.initial_balance` stays refused: it would be a second owner of the number
  D7 just gave one owner.

### 10.2 Never materialize

Follows from 10.1. Synthesizing `"prop"` into `config_data` would arm or disarm
the survival guard -- for every user, on next save -- which is exactly what D7
and the explicit-`enabled` rule exist to prevent. (`startup_probe` is harmless to
synthesize now: its fields default to whatever `ProbeCriteria::Development()`
says, so an absent block and a present block mean the same thing.
`trade_management.structural_exit` is equally harmless: its resolver treats an
absent block and an `enabled: false` block identically, and defaults to off --
the opposite polarity from `prop`. Both stay out of `config_data` because D4 is a
blanket rule, not because they are dangerous.)

### 10.3 Toggle crashes on an absent key

```cpp
config_data[row.ptr] = !config_data[row.ptr].get<bool>();
```
`json::const_operator[]` on a missing key throws. Today it works only because the
key is in the file. The first schema-only field turns this into a crash. Fix in
P2 (one branch, per 8).

### 10.4 Load writes without a keystroke

Two writes in `LoadConfigSmart`: the `needs_save` repair path (missing or
unparseable file), and the unconditional
`config_data["backtest"]["config_profile"] = config_path;`. Any
"normalize on load" scheme compounds this into "the file changed and I did not
touch it". Removing the `config_profile` key (D6) deletes the second; P2 deletes
the first.

### 10.5 `config_data["backtest"]` assumes an object

The assignment above throws out of `PageConfig`'s constructor -- outside the
existing `try` -- if the file is valid JSON but `"backtest"` is not an object, or
the file's root is not an object. Gone with D6; check the root type explicitly
while it is still there.

### 10.6 A key named `error` is unparseable

```cpp
if (config_data.contains("error")) throw std::runtime_error("corrupt json");
```
A legitimate `"error"` key (or a profile an external tool wrote with an `error`
field) permanently looks corrupt and gets replaced by defaults + saved. Remove.

### 10.7 File selectors detected by substring

`current_path.find("dataset")` and `find("config_profile")` mean any future key
containing "dataset" becomes a directory picker. Also
`ScanDirectory` pushes `NO_DATASETS_FOUND` / `NO_FILES_FOUND` into the option list,
which the user can select and which then gets written as a path. Replace with
`kind == DirPath/FilePath` + `selector_root` from the table, and keep the
sentinel out of the writable list.

### 10.8 `max_cores: 6.0` is ignored

`env["max_cores"].is_number_integer()` is false for a JSON float, so the config
silently gets `hardware_concurrency()`. Table-driven `GetInt` with an integer
coercion rule fixes it; this specific file is wrong today.

### 10.9 Grid predicates disagree

See 9.

### 10.10 `Render` mutates state

See 8. Cosmetic today, but it is exactly the kind of thing that makes a
refactored menu behave differently on the first frame after a profile switch.

### 10.11 Hardcoded columns and red-on-darkred text

Value column starts at 75; longer labels truncate (8). The inline edit buffer is
drawn with `StyleTextDefault` (255,10,20) over `StyleEditBackground` (150,10,30)
-- red on dark red. Use `StyleDataValue` (cyan) for edited text.

### 10.12 Component arrays cannot be tabled

Scope note, not a bug (5). Say so in the doc so nobody assumes "schema complete"
means "every key".

### 10.13 Every removed spelling

D1 keeps one key per field, so every alias in the current loader is deleted
rather than deprecated. A profile carrying any left-hand key fails to load with
`config error: unknown key '<path>'` plus the nearest known key, and never falls
back to a default. `configs/` is rewritten after P1b, so nothing is migrated.

| removed | replacement |
|---|---|
| `risk.sl_price_pct` | `risk.sl_pct` |
| `risk.tp_pct` (absolute) | `risk.tp_r` (multiple of 1R) |
| `risk.max_daily_loss` | already ignored; now rejected |
| `environment.filepath` | `environment.dataset` |
| `venue.env` | `venue.environment` |
| root `is_testnet` | `venue.environment: "testnet"` (struct field stays, derived) |
| root `data_exchange` | `venue.data_exchange` |
| `data_venue.{exchange,environment}` (root or under `venue`) | `venue.data_exchange`, `venue.data_environment` |
| `trade_management.*.trigger_pct` / `target_pct` / `distance_pct` | the `_r` forms |
| `exchange` as an object (`{"name": ...}` or `{"venue": ...}`) | root string `exchange` |
| `environment.exchange` (fallback when root is absent) | root `exchange` |
| root `category` / `symbol` / `allow_mainnet` | `venue.category` / `venue.symbol` / `venue.allow_mainnet` |
| credentials under `exchange` / `environment` | root `api_key`, `api_secret`, `api_passphrase` |
| `execution.*` falling back to `environment.*` | `execution.*` only |
| `strategy.triggers` / `strategy.filters` | root `triggers` / `filters` |

Consequences worth naming:

- `trade_management.hpp` holds three resolvers with legacy branches to delete:

  - `ResolveRiskPcts` reads `sl_pct` then `sl_price_pct`, and `tp_r` then the
    legacy absolute `tp_pct`. Drops to `sl_pct` / `tp_r` only.
  - `ResolveTpPct` reads `tp_r` then `tp_pct`. Drops to `tp_r` only. Its
    "required" `throw` becomes unreachable once `risk.tp_r` is `kRequired`
    (D7) -- keep it anyway; the schema test and the resolver cannot drift if
    both check, and it is one line.
  - `ResolveLevelPct` reads `_r` then `_pct`. Drops to `_r` only.

- **`ResolveRiskPcts` and `ResolveTpPct` compute the same `sl` / `tp` pair from
  the same json.** That is the one-owner problem named for `initial_balance`,
  applied to `sl_pct` / `tp_r` -- and the header comments themselves note this
  pair has resolved to different values in different readers before. Pick one in
  P1b: either delete `ResolveRiskPcts` and have its callers read
  `AppConfig.sl_pct` / `AppConfig.tp_pct`, or keep it and make `ResolveTpPct` a
  call to it. Not both.
- `tp_pct` and `tm_config.*_pct` stay as fields but become *derived*
  (`tp_pct = tp_r * sl_pct`), never readable keys.
- `AppConfig::is_testnet` stays a derived field (`env != Mainnet`) because the
  broker reads it, but no config can set it directly any more.
- `ResolveStructuralExitConfig`'s own `read_enabled` accepts `enabled` /
  `active` / `isActive`. That spelling choice lives inside
  `structural_exit.rules[]`, which is out of scope (section 5), so it is
  unchanged in v1.

The unknown-key report (Q4) is what makes this safe in practice: a stale profile
fails on the first run instead of half-applying.

## 11. Optional cleanups (do only if touching that code anyway)

- `LoadConfigSmart` does five things in one function: load, repair+save, write the
  profile pointer, flatten, clamp cursors. Split into `LoadFrom(path)` (read-only)
  and `SetActiveProfile(path)` (UI state). Makes P3 much smaller.
- The `config_profile` `INLINE_EDIT` branch (save, swap path, reload) becomes a
  dedicated menu action. Today it is reachable only through a row that D6
  deletes, so the action needs a key binding or a header line; decide in P2.
- `MenuRow`'s four type bools collapse to `def->kind` (8).
- `SaveAndReload()` reloads from disk after every toggle, which re-flattens the
  whole menu. Toggling is a memory write; re-render, do not re-read.
- Fix 10.6, 10.7, 10.8 independently, before any of this. They are small, real,
  and shippable today.
- `ResolveTpPct` / `ResolveLevelPct` collapse to the R-only path (10.13).
  `ResolveRiskPcts` is deleted or becomes the single reader there -- see 10.13.

## 12. Phases

**P0 -- standalone bug fixes.** 10.6, 10.7 (guard the sentinel), 10.8, 10.11.
No structural change. Shippable immediately, and shrinks the later diff.

**P1 -- schema module, unconsumed.**
Add `config_schema.{hpp,cpp}` with the table + `Find` + typed getters + `Has` +
`IsGridNode`. Nothing calls it yet.
*Done when:* every entry has a path that resolves, a default of the declared kind,
an in-domain default, and a unique path. Tests in 13.1--13.3 pass.

**P1b -- `Load` reads through the table.**
Replace `SafeNumber`/`SafeBool`/`SafeString`/`RequireNumber` call sites with
`cfg::GetX`; `AppConfig` member initializers become `= {}`; presence checks go
through `Has`; `prop` requires an explicit `enabled`.
Unknown keys FAIL the load (10.13); see 14.4 for why the WARN plan was dropped.
*Done when:* 13.4--13.7 pass, and every profile in `configs/` either loads with
identical effective values or fails naming the offending key -- a 10.13 removed
spelling, a missing required key, or an implicit `prop.enabled`. Profiles are
expected to fail here; they are rebuilt afterwards.

**P2 -- UI reads the table.**
Menu built from `Schema()`; `MenuRow` reduced; toggle creates absent keys;
selectors driven by `kind`; `Render` read-only; load/save split.
*Done when:* a field added to the table appears in the TUI with its default and
can be toggled, with no config file edited; a key removed from every profile
still renders; and the three sources coexist in one menu -- a schema row
(`risk.tp_r`), a component row (`triggers[1].ratio`), and a file-driven row
(`backtest.wfa_mode`) all render, in that order, under their own categories.

**P3 -- retire the file schema.**
Delete `configs/_defaults.json` and `LoadDefaults()`; move the profile picker to
session state (D6).
*Done when:* `grep -r _defaults` is empty, and a profile holding nothing but the
D7 required keys loads, renders every optional row with its table default, and
completes a run.

**P4 -- diff/export (optional).** "Export full profile". Skip until asked for.

P0, P1 and P1b are independently useful. Nothing forces P3.

## 13. Tests

1. **Table self-check** -- unique paths; `Find(path) == &entry`; `kind`/default
   type agree; default inside `[min,max]`; `Enum` default is in `enum_values`;
   `Dir/FilePath` has `selector_root`.
2. **Minimal input** -- a config carrying only the D7 required keys loads; every
   optional field equals its table default; `Has(min, p) == false` for every
   optional path. `Load({}, false)` fails naming `exchange` (the first required
   key by schema order), not a substituted default.
3. **Round trip** -- `Load(required-keys + every table default)` and
   `Load(required-keys only)` produce identical `AppConfig`. Catches a default
   written in the wrong place.
4. **Presence** -- `prop` block without `enabled` throws with the field name;
   `prop.enabled: false` plus no `initial_balance` still fails on the required
   field; `prop.enabled: true` without it names `initial_balance` too (10.1b is
   deleted by D7, so both paths report the same missing key).
5. **Removed spellings** -- every row of 10.13, one case each: the load fails with
   `unknown key` naming the path, and the value does not fall back to the default.
   `risk.sl_price_pct` and `environment.filepath` are the two worth naming in the
   test because they are the ones old profiles actually carry.
6. **Grid** -- `{min,max,step,val}` on a `kSweepable` field resolves to `val`;
   `{min,max,step}` without `val` is an error naming `val`; a grid node on a
   non-sweepable field is an error.
7. **Domain** -- out-of-range `prop.warn_fraction` fails with the path;
   `exchange: "binance"` fails. The getter's `enum_values` check fires first
   ("must be one of bybit|hyperliquid|blofin"), so the depth-1 explanation stays
   in `Validate()`'s comment rather than in the error text; the test asserts the
   venue list, not the wording.
8. **UI, headless** -- build the menu row list and assert:
   (a) a field absent from `config_data` renders its default and toggles without
   throwing;
   (b) a config with all three source kinds -- a schema scalar (`risk.sl_pct`), a
   component array (`triggers`), and a file-driven subtree (`backtest`) -- yields
   exactly one row per intended path, no duplicates, and no row for a deliberately
   misspelled `risk.slipage_pct` (that one is a lint, not a row);
   (c) a component-array grid node (`triggers[1].levels`) renders as a range,
   which is the case `kSweepable` must not gate (§9).

## 14. Resolved

The five questions this section used to hold are answered, and the answers are
folded into the decisions above.

1. **Aliases** -- none kept, no compatibility layer. Profiles are rebuilt, not
   migrated (D1, 10.13).
2. **`initial_balance`** -- one owner, required unconditionally (D7). The
   conditional prop check and `prop.initial_balance` both disappear.
3. **`environment.dataset`** -- table default `"data/"`; the default only works
   after the `DataManager` change below.
4. **Unknown-key reporting** -- a load error naming the dotted path, no TUI
   screen (10.13). The WARN plan was dropped during P1b: warn-then-continue lets
   a typo like `risk.slipage_pct` tune nothing while the run looks healthy, which
   is the failure D1 exists to prevent. `cfg::RejectUnknownKeys` throws; the
   nearest-match hint is still future work, and is a convenience, not a reason to
   keep loading.
5. **`backtest.*` and per-component schema** -- out of v1 (section 5).

Resolved by reading `data_manager.hpp`:

- **`environment.dataset = "data/"` needed a small `DataManager` change.**
  **Done (§15.9).** `ResolveDatasetRoot` maps the argument to one dataset
  before any loading happens: a recording or a directory holding
  `trades.csv` / `L2.csv` / a cache is returned unchanged, and anything else
  that is a directory is treated as a root and resolves to the newest dataset
  beneath it. Two details that are easy to get wrong and are pinned by tests:

  - resolution happens **before** the `IsSessionFile` check, not after. A root
    that resolves to a `.mkn` must reach `ReadSessionFileRange`; checking the
    unresolved path first would send it down the directory branch, which loads
    the whole file and trims in memory -- the freeze range reads exist to stop.
  - `IsSessionRecording` resolves too, because the runners branch on it to
    choose a streaming book source.

  "Newest" is by **path**, not `last_write_time`: the layout puts the date in
  the name, and mtime is rewritten by a copy, a checkout or an rsync. A root
  holding nothing still returns an empty channel rather than an error -- that
  result is honest for a directory that may not be populated yet, and the
  resolver's job is to find a dataset when one exists.
- **`LoadAllRange` is unaffected.** It works on the same argument, so it inherits
  whatever `LoadAll` resolves.
- **The empty-vs-error distinction is worth preserving.** A recording that is
  legitimately trades-only returns empty L2; that path must keep working.

## 15. P1b -- the edit list

`AppConfig::Load` stops being a reader and becomes a *consumer* of the table.
Each numbered item is independently compilable and testable; do them in order,
and `configs/default.json` should load with identical effective values after
every one except where marked **behaviour change**.

### 15.1 Preparation

1. `#include "config_schema.hpp"` in `src/config_parser.cpp`.
2. Delete `SafeNumber`, `SafeBool`, `SafeString`, `RequireNumber`, `RequireBool`,
   `RequireInt`, `RequireNumberAny`. They exist only to supply a fallback
   argument, which D2 forbids. If a call site resists deletion, the table is
   missing a field -- add it there instead.
3. `AppConfig` member initializers become `= {}`. The table owns every default
   now; two owners is the bug this project removes.

   **Not done, deliberately.** `AppConfig` keeps its initializers, and two
   disagree with the table: `sl_pct` 0.002 vs `risk.sl_pct` 0.003, and
   `risk_per_trade_pct` 0.01 vs 0.005. Both keys are `kRequired`, so `Load`
   cannot reach the struct values -- only a directly-constructed `AppConfig`
   sees them, which `test/test_config.cpp` pins on purpose. Deleting them is a
   live decision, not a cleanup: the struct defaults are now "what tooling gets
   when it skips `Load`", and §13.3 already proves a *loaded* config takes
   everything from the table. Rescope this item to "initializers stay, for the
   paths `Load` cannot reach" rather than deleting them.

### 15.2 Removed spellings (10.13), one at a time

Delete each read; the key becomes an unknown key that fails the load. Run the
test fixture after each.

4. `environment.filepath` -- `dataset` is the only spelling.
5. `venue.env` -- `venue.environment` only.
6. `risk.sl_price_pct`, `risk.tp_pct` -- via the resolver changes in 15.7.
7. `risk.max_daily_loss` -- delete the "silently ignored" read entirely.
   **Behaviour change**: a profile carrying it now fails instead of loading.
8. `exchange` as an object (`{"name":...}`, `{"venue":...}`), and
   `environment.exchange` as a fallback -- root string `exchange` only.
9. Root `category` / `symbol` / `allow_mainnet` -- `venue.*` only.
10. Credentials under `exchange` / `environment` -- root only. The env-var
    fallback stays; that is not a config key.
11. `execution.*` falling back to `environment.*` -- `execution.*` only.
12. `strategy.triggers` / `strategy.filters` -- root `triggers` / `filters` only.
13. The four `data_venue` spellings -- `venue.data_exchange` /
    `venue.data_environment`, with empty meaning "same as the account".

### 15.3 Derived and promoted fields

14. `is_testnet` is currently assigned **twice**: once from root `is_testnet`
    (or nested under `exchange`/`environment`), then overwritten from
    `venue.environment`. Delete the first assignment. Keep the derived field
    (`venue.environment != "mainnet"`) because brokers read it, but nothing in a
    config can set it.
15. `initial_balance`: `cfg::GetDouble` as a required field. Delete
    `has_initial_balance` and the conditional prop throw with it (10.1b). Keep
    one explicit error: `prop.initial_balance` is still refused, because it
    would be a second owner of the same number.
16. `environment.max_cores`: `cfg::GetInt`. `"max_cores": 6.0` now yields 6
    instead of silently becoming `hardware_concurrency()` (10.8). Keep `0` as
    the auto sentinel.

### 15.4 `prop` -- the one real toggle

17. Read the block only when present: `if (cfg::Has(src, "prop")) { ... }`.
18. Inside it, require an explicit `enabled`:

    ```cpp
    if (!cfg::Has(src, "prop.enabled")) {
      throw std::runtime_error(
          "config error: 'prop' present but 'prop.enabled' is missing; "
          "add it explicitly (true to arm, false to disable)");
    }
    ```

    This is the whole of 10.1a. Do **not** fall back to `false`: a profile whose
    author wrote the block to arm it must not load silently disarmed.
19. The four `R*` fields are read only when `enabled` is true, through the
    table.

### 15.5 `trade_management` -- four blocks, four policies

20. `break_even.enabled` optional (table default `true`); `trailing.enabled` and
    `scale_out.enabled` `kRequired`; `structural_exit.enabled` optional
    (default `false`). The table already encodes all four; the edit is deleting
    the ad-hoc `SafeBool(..., true)` / `RequireBool` split in the loader.
21. `structural_exit.min_holdtime_ms` stays **conditionally required** on
    `enabled`. `kRequired` cannot express that, so it remains a cross-field
    check after the table read. Leave a comment saying so, or someone will
    "simplify" it away. Same shape: at least one non-disabled rule when
    `structural_exit.enabled`.
22. Delete every `_pct` read (`trigger_pct`, `target_pct`, `distance_pct`).

### 15.6 `startup_probe` -- the one hole the design does not cover

**Decision needed before A.5 can be finished.** `preset` is a *cascade*:
`"preset": "production"` means "every key I did not write takes its value from
`ProbeCriteria::Production()`". A flat table can express one default per field,
not "default depends on a sibling". Section 4 rejects a `default_fn` hook, and
correctly so.

Pick one:

- **(a) Special-case in `Load` (recommended).** Read `preset` first, then read
  the six numbers against the table *and* use the chosen `ProbeCriteria` when
  the key is absent. Six lines, one comment, no descriptor change. The table's
  defaults for those fields are `Development()`'s values, so `preset` absent and
  `preset: development` keep meaning the same thing.
- **(b) Drop the cascade.** `preset` only picks the derived values, and the six
  numeric keys always need to be explicit. Cleaner table, worse ergonomics,
  changes what `production` means today.

Either way, `ProbeCriteria`'s real numbers must be transcribed into the table --
the six `?` rows in 5.1 are placeholders until then.

### 15.7 One owner for `sl_pct` / `tp_r`

`ResolveRiskPcts` and `ResolveTpPct` compute the same pair from the same json,
and the header comments note the pair has resolved differently in different
readers before. Pick one:

- **(a) `ResolveRiskPcts` is the only reader (recommended).**
  `AppConfig::Load` calls it once and stores both numbers; `ResolveTpPct`
  becomes a one-line call to it. Every other caller keeps working.
- **(b) Delete `ResolveRiskPcts`** and have its callers read
  `AppConfig.sl_pct` / `AppConfig.tp_pct`. Bigger diff, touches live paths.

Also in `trade_management.hpp`: `ResolveLevelPct` drops its `_pct` branch.

### 15.8 Enforcement and lint

23. A grid node on a non-`kSweepable` field is a load error naming the path
    (section 9). `FlattenGrids` calls `cfg::IsGridNode` (shape only) -- do not
    pass it a `Field`.
24. Unknown-key check (`cfg::RejectUnknownKeys`, implemented in P1b): walk the
    config; a leaf path that is not `cfg::Find(...)` and not under an allowed
    subtree (components, `backtest`, `structural_exit.rules`) throws naming it.
    Fails the load (section 14 item 4).
25. A `{min,max,step}` node with no `val` is an error naming `val`.

### 15.9 `DataManager` (same phase)

26. `LoadAll` / `LoadAllRange` accept the data root and resolve it to the newest
    dataset (newest matching subdirectory, or newest `.mkn` under it).
    **Done** via `DataManager::ResolveDatasetRoot`, applied before the
    recording check in all four entry points and in `IsSessionRecording`.
    `test/test_dataset_root.cpp` covers the newest-by-name rule (with the
    older-dated directory written last, so an mtime-based resolver fails it),
    both channels resolving to the same day, "a dataset is a leaf", and the
    preserved empty-root behaviour.

### 15.10 Done when

Every profile in `configs/` either loads with identical effective values, or
fails naming the offending key (a removed spelling, a missing required key, or an
implicit `prop.enabled`). Profiles are expected to fail here; they are rebuilt
afterwards.

**Status:** done, and now enforced by `test/test_profiles.cpp` rather than by a
manual sweep. Per profile:

- `configs/default.json` -- loads unedited. One intended change in effective
  values: `max_cores: 6.0` now yields **6** instead of falling back to
  `hardware_concurrency()` (10.8).
- `configs/backtest.json` -- loads unedited. `preset: "production"` reaches the
  cascade (15.6a), and `risk.sl_pct` plus three trigger params are grid nodes;
  only `sl_pct` has a `Field`, which is why the shape predicate must stay
  schema-free (section 9).
- `configs/backtest_structural_exit.json` -- one fix: root `is_testnet`
  deleted (10.13; `venue.environment: "mainnet"` already says it). The
  `ImbalanceBreak.{min_levels,ratio}` grids are untouched: `rules` is an
  allowed subtree, which is what section 5 promised.
- `configs/default.json.bak` -- two fixes applied: root `is_testnet` and
  `risk.max_daily_loss` deleted.
- `configs/_defaults.json` -- loads unedited, and must keep loading until P3
  deletes `LoadDefaults()`.

Every profile still needs credentials in the environment (or a `--paper` run)
before it can place an order, since `Validate` requires them for bybit; that is
not a schema concern. `backtest.config_profile` stays in all of them until P2
deletes the menu row that writes it (D6).

## 16. The screen

Section 8 specified the data source, not the visuals. This is the visual spec.
It is a *minimal* change: same three-panel layout, same key handling, four
additions.

### 16.1 Today

```
                     HEADER
  CATEGORY        SUBCATEGORY     FIELD          VALUE
> Risk            General         sl_pct         0.004
  Risk            General         tp_r           2.0
  ...
                     FOOTER
```

Columns at 2 / 21 / 43 / 45 / 47 / 75 (`ui_menu.hpp`). Order is
alphabetical-by-JSON-key, i.e. accidental. A key absent from the file has no row
at all.

### 16.2 After

```
  MOKUREN   configs/default.json                         [modified] *

   CATEGORY          SUBCATEGORY     FIELD              VALUE

 > Account           General         exchange           bybit
   Account           Credentials     api_key            ******
   Environment       General         dataset            data/
   Environment       General         interval_ms        60000
   ...
   Risk              General         sl_pct             0.004
   Risk              General         tp_r               [1.0->3.0*0.5=2]
   ...
   Prop              General         enabled            off
   Prop              General         max_daily_dd_pct   0.04
   ...
   Trade Management  Structural exit enabled            off
   Trade Management  Structural exit min_holdtime_ms    180000

  ENTER toggle/edit  <- -> step  TAB panel  S save  P profile  Q quit
  2 unknown keys in this profile (see log)
```

Four additions, all in `Render`:

1. **Absent keys render dimmed.** A value that is *not* in the file (coming from
   the table default) draws in a muted colour; a value the file pins draws
   normal. This is D4 made visible: at a glance, what the profile actually says
   versus what it inherits. Header shows `[modified] *` when `config_data`
   differs from what was loaded.
2. **Grid values render inline**: `[1.0->3.0*0.5=2]` (min->max*step=val). Past
   ~24 chars, truncate with `...` and open the editor for the full view.
3. **Footer gains a profile line** with the unknown-key count. It is a count, not
   a review screen -- section 14 rejected the screen.
4. **The inline edit buffer uses `StyleDataValue`** (cyan) over
   `StyleEditBackground`, not `StyleTextDefault` (10.11).

Unchanged on purpose: three panels, `TAB` to change panel, same row highlight,
same up/down.

### 16.3 Row types, as they will look

| kind | display | edit |
|---|---|---|
| Bool | `on` / `off` | `ENTER` toggles (writes the default first if absent) |
| Int / Double | `60000` / `0.004` | `<- ->` steps, `ENTER` inline edit |
| Enum | `bybit` | `<- ->` cycles `enum_values`; no free text |
| String | text | `ENTER` inline edit |
| DirPath / FilePath | path, truncated from the left | `ENTER` opens the picker |
| grid node | `[1.0->3.0*0.5=2]` | `ENTER` opens the 4-row editor |
| component row | as today | as today |
| file-driven row | as today | as today |

The picker lists `ScanDirectory(selector_root, only_dirs)` and nothing else -- no
`NO_DATASETS_FOUND` row (10.7). An empty list means `ENTER` does nothing and the
footer says why.

### 16.4 Where new fields appear

Add a `Field` to the table with a `category`/`order` -> it renders in that panel
on the next start, dimmed (it is not in the file), and `ENTER` writes it in. That
is the whole point of the project, and this layout is what makes it visible
rather than confusing: a dimmed row obviously means "you have not set this", not
"this is set to a weird value".

## 17. Open UI decisions

Three, all small. Leans given; none blocks starting.

1. **Three panels, or a flat list?** Keep three. The layout is already there,
   `TAB` already works, and section 8's category ordering slots into the left
   panel without a redesign. A flat list with a breadcrumb is nicer at 40 rows
   and worse at 200; this config will pass 100.
2. **Which rows offer "make this a sweep"?** Only `kSweepable` schema rows.
   Component rows keep today's behaviour (they are already swept in existing
   profiles). A grid node on a non-sweepable row still *renders* as a grid so it
   can be edited back, per section 9.
3. **Profile picker binding.** `P`, opening a selector over `configs/*.json`. D6
   deleted the row that used to do this, so it needs a home; a footer hint is the
   cheapest honest one.

## 18. P2 preview

Not detailed yet -- it depends on section 16 being agreed. In outline:
`BuildMenu()` replaces `FlattenJson`'s discovery loop (three sources, 8.2);
`MenuRow` reduces to `{def, is_selector, options, current_idx}`; toggle creates
absent keys; selectors driven by `kind`; `Render` becomes read-only (cursor
clamping moves to `HandleInput` + load); `LoadConfigSmart` splits into `LoadFrom`
(read-only) and `SetActiveProfile`.

## 19. Before starting P1b

Done -- kept as the derivation record for the table's defaults. Section 5.2
lists what was unread at the time; this was the subset that blocked the first
working day, in order.

- `include/startup_gate.hpp` -- 15.6 cannot be finished without
  `ProbeCriteria`'s real numbers.
- `include/prop_guard.hpp` -- fills the last two `?` rows in 5.1.
- `include/venue_spec.hpp` -- confirms `enum_values` are the loader's strings.
- `src/data_manager.cpp` -- 15.9.
- `include/ui_pages.hpp`, `include/ui_manager.hpp`, `src/ui_manager.cpp` -- P2,
  but reading them early tells you whether 16.2's footer line and `[modified]`
  marker have a home (`UIPage` may already provide a status slot).

Start with 15.2 (the removed spellings): it is mechanical, each item is a
compile-and-test cycle, and it is the part most likely to surface a profile in
`configs/` that is still carrying an old key.

## 20. Not doing

- No runtime schema file. Compiled in, per the brief; a separate artifact is one
  more thing to lose and it trades a compile error for a startup error.
- No X-macros / reflection. The table does not need to also generate the struct.
- No config layering beyond profile >= default. No env-var overrides, no
  `--set key=value`.
- No config versioning/migration machinery. Profiles are rewritten by hand after
  P1b, and a stale one fails loudly on the offending key (10.13) instead of
  half-applying.
- No aliases, no deprecation warnings, no "did you mean" auto-fix. The hint is
  text in the error, not a rewrite of the file.
- No removal of `FlattenGrids`, `GridScanner`, or `Validate`.
