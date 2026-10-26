# Installation

Mokuren is built with CMake and targets Linux with a C++17 compiler.

## Requirements

| requirement | used for | resolved by |
| ----------- | -------- | ----------- |
| CMake 3.14+ | the build itself | — |
| a C++17 compiler (GCC 9+, Clang 10+) | — | — |
| pkg-config | the two below | `PkgConfig` |
| hiredis | redis pub/sub for live events | pkg-config `hiredis` |
| notcurses++ | the interactive TUI | pkg-config `notcurses++` |
| OpenSSL | HMAC-SHA256 request signing, TLS | `find_package(OpenSSL)` |
| libcurl | authenticated REST calls | `find_package(CURL)` |

Package names vary by distribution:

```bash
# Arch
sudo pacman -S --needed base-devel cmake pkgconf openssl curl hiredis notcurses

# Debian / Ubuntu
sudo apt install build-essential cmake pkg-config libssl-dev \
  libcurl4-openssl-dev libhiredis-dev libnotcurses-dev

# Fedora
sudo dnf install gcc-c++ cmake pkgconf-pkg-config openssl-devel \
  libcurl-devel hiredis-devel notcurses-devel
```

Both pkg-config modules are hard requirements: `pkg_check_modules(... REQUIRED
IMPORTED_TARGET ...)` fails the configure step outright if `notcurses++` or
`hiredis` cannot be found. If `pkg-config --exists notcurses++` fails, the C++
bindings are packaged separately on your distribution and must be installed too.

## Fetched at configure time

| dependency | version | role |
| ---------- | ------- | ---- |
| nlohmann/json | 3.11.3 | config parsing |
| spdlog | v1.14.1 | async logging |
| ixwebsocket | v11.4.5, TLS enabled | live market data |
| Catch2 | v3.5.2 | test framework |

The first configure downloads them, so it needs network access. Afterwards the
sources are cached under `build/_deps` and reconfiguring is offline.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Use `Release`. The project sets no default `CMAKE_BUILD_TYPE`, so an
unoptimised build is both slow and easy to mistake for a strategy problem.

Source files are picked up by a recursive glob with `CONFIGURE_DEPENDS`, so a
new `.cpp` under `src/` is compiled without re-running CMake by hand. The two
`src/market_data/` translation units are also named explicitly and removed from
the glob first, so each is compiled exactly once: before that, `engine_tests`
silently omitted them and the only symptom was undefined `mokuren::mdf::*` at
link time.

## Binaries

| binary | built from | purpose |
| ------ | ---------- | ------- |
| `build/mokuren` | `src/` + `src/main.cpp` | TUI backtesting, `--live`, `--paper`, `--probe` |
| `build/engine_tests` | `test/` + `src/` without `main.cpp` | the only test runner |
| `build/mokuren_record` | `tools/record.cpp` + `src/` | record a venue feed into `.mkn` |
| `build/mokuren_inspect` | `tools/inspect.cpp` + `src/` | decode a dataset through the replay entry points |

The three tools link the engine sources, not a trimmed reader. That is
deliberate: a recording and a verification are only meaningful if they exercise
the same parser and the same loader that trade.

## Tests

```bash
./build/engine_tests
```

There is no `enable_testing()` and no ctest registration — by design, so there
is exactly one source of truth for pass/fail. Cases can be narrowed by name or
tag:

```bash
./build/engine_tests "*router*"
```

Network cases are hidden from Catch2 and opt-in, so the plain run never opens a
socket:

```bash
./build/engine_tests "[.live]"
```

## Runtime paths

All paths are relative to the working directory you start the binary from.

| path | written by |
| ---- | ---------- |
| `logs/engine.log` | `mokuren` (parent directory created if missing) |
| `logs/inspect.log` | `mokuren_inspect` |
| `live_debug.log` | `mokuren --debug` |
| `recordings/` | default output directory of `mokuren_record` |

Start from the repository root, or from whichever directory should own `logs/`
and `recordings/`. A daemon's `WorkingDirectory` decides this — see
[Running as a daemon](deployment.md).
