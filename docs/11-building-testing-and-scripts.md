# 11. Building, testing & scripts

[← Operational model](08-operational-model.md) · [Back to index](README.md)

Everything you need to build the daemon, run the test suite, and use the helper
scripts. Those scripts live in `scripts/` and are thin, readable wrappers over
`cmake`/`ctest`, so you can always run the underlying commands yourself — and every
script that takes options accepts `--help`.

## 11.1 Prerequisites

- A C++20 toolchain (gcc ≥ 11 or clang ≥ 14).
- CMake and Ninja.
- Most third-party C++ dependencies (Drogon, nlohmann/json, libxml2, GoogleTest) are
  fetched and pinned automatically at configure time; a few come from system
  packages — see the **System dependencies** section of the project `README.md`.
- Optional: `clang-format` (for `format.sh`), plus Node and `pnpm` (only if you're
  building the dashboard).

## 11.2 Build

```sh
./scripts/build.sh                          # configure + build a Debug tree into build/
./scripts/build.sh -DAID_WERROR=OFF         # any extra args pass straight to cmake
./scripts/build.sh -DCMAKE_BUILD_TYPE=Release
```

`build.sh` configures a Ninja Debug build in `build/`, compiles it across all cores,
and symlinks `compile_commands.json` into the project root so editors and clangd
pick it up. Re-run it whenever you like — CMake reconfigures incrementally.
Everything after the script name is forwarded verbatim to the `cmake` configure step,
so you can flip any option or build type from the command line.

The same thing by hand:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j
```

### CMake options

Here's the complete set of project options — all of them default sensibly:

| Option | Default | Effect |
|---|---|---|
| `AID_BUILD_TESTS` | `ON` | build the GoogleTest suite (`OFF` to skip tests entirely) |
| `AID_SANITIZE` | `OFF` | ASan + UBSan instrumentation (prefer `sanitize.sh`, which sets it) |
| `AID_WERROR` | `ON` | treat compiler warnings as errors |

Standard CMake variables apply too — `-DCMAKE_BUILD_TYPE=Debug|Release|RelWithDebInfo`,
`-DCMAKE_CXX_COMPILER=clang++`, and so on. The daemon binary lands at `build/src/aid`;
for how to run it, see [Getting started §10.2](10-getting-started.md).

## 11.3 Run the tests

```sh
./scripts/test.sh                  # build if needed, then run the whole suite
./scripts/test.sh -j8              # everything after the name passes to ctest
```

`test.sh` rebuilds if it has to, then runs `ctest --test-dir build
--output-on-failure`. The suite is one test executable per layer under `tests/`
(`domain/`, `usecases/`, `controllers/`, `infrastructure/`, `adapters/`, `auth/`,
`plumbing/`, `ports/`, `value-types/`, `crosscutting/`, `integration/`, and so on),
each registered with CTest through GoogleTest discovery — so any individual test case
is selectable by name.

### `ctest` recipes

Any `ctest` argument works after `./scripts/test.sh`, or you can run `ctest
--test-dir build` directly:

| Goal | Command |
|---|---|
| Run everything | `./scripts/test.sh` |
| One test/case by name (regex) | `./scripts/test.sh -R CallTracker` |
| Exclude tests by name (regex) | `./scripts/test.sh -E SlowThing` |
| Only a label | `./scripts/test.sh -L integration` |
| Everything **except** a label | `./scripts/test.sh -LE slow` |
| List tests, don't run | `ctest --test-dir build -N` |
| Run in parallel | `./scripts/test.sh -j8` |
| Verbose / very verbose | `./scripts/test.sh -V` / `-VV` |
| Re-run only last failures | `./scripts/test.sh --rerun-failed` |
| Stop at first failure | `./scripts/test.sh --stop-on-failure` |
| Hunt a flake (repeat) | `./scripts/test.sh -R Foo --repeat until-fail:50` |
| Per-test timeout (sec) | `./scripts/test.sh --timeout 30` |

You can also drive a test binary directly when you want GoogleTest-native flags:

```sh
build/tests/domain/aid_domain_tests --gtest_list_tests          # list cases
build/tests/domain/aid_domain_tests --gtest_filter='CallTracker.*'
build/tests/domain/aid_domain_tests --gtest_filter='CallTracker.*' --gtest_repeat=100 --gtest_shuffle
```

### Test labels

| Label | Marks | Run by default? |
|---|---|---|
| `slow`, `integration` | end-to-end tests over the fake ports | yes — skip with `-LE slow` |
| `live` | a forked-daemon E2E test needing a real dev environment (running backend + config) | **no** — self-skips |

The `live` test is gated twice over: it's selected only by `-L live`, and *armed*
only when `AID_LIVE_E2E=1`. Missing either one, it exits as a clean skip, so a normal
`./scripts/test.sh` never touches it. To run it against a prepared dev environment:

```sh
AID_LIVE_E2E=1 ctest --test-dir build -L live --output-on-failure
```

Even then it self-skips (never fails) if the daemon binary or config is missing.

### Sanitizers

```sh
./scripts/sanitize.sh              # ASan + UBSan build in build-asan/, then run the suite
```

This configures a separate `build-asan/` tree with `AID_SANITIZE=ON`, builds it, and
runs the tests with the sanitizers active (the harness sets `ASAN_OPTIONS` /
`UBSAN_OPTIONS` per test, so you don't have to). It takes no arguments. Keeping it in
its own tree keeps your normal `build/` fast; run it before committing any change
that touches pointers, lifetimes, threads, or coroutines.

## 11.4 Formatting

```sh
./scripts/format.sh                # clang-format -i across src/ lib/ tests/ include/
./scripts/format.sh --check        # dry-run; non-zero exit if anything is unformatted
```

`--check` is the CI / pre-commit mode — it fails on any unformatted file — while the
default rewrites in place. Either way you need `clang-format` on your `PATH`.

## 11.5 Build the dashboard

```sh
./scripts/build-ui.sh              # builds the SvelteKit SPA into ui/build/
```

Needs only Node and `pnpm`, and takes no arguments. The dashboard is a build-time
concern kept separate from the CMake build — production ships the static bundle, not
Node. Once it's built, point the daemon's `Ui.documentRoot` at the *absolute* path of
`ui/build/` (see [Configuration](07-configuration.md)).

## 11.6 Local deploy

```sh
./scripts/deploy.sh                # rebuild + install daemon and both plugins together
./scripts/deploy.sh --no-restart   # install without restarting the running daemon
AID_DEPLOY_ROOT=/opt/aid ./scripts/deploy.sh   # override the deploy root
```

`deploy.sh` rebuilds and installs the daemon binary *and* both plugin `.so` files as
one atomic step, into a deploy root (`AID_DEPLOY_ROOT`, default `~/aid-dev`, which
mirrors the production layout: `plugins/`, `etc/config.json`). Shipping them together
keeps the daemon and a plugin from drifting apart — and if they somehow do, the
ABI/contract-tag guards ([§5.3](05-writing-a-plugin.md)) catch the mismatch up front
rather than at runtime. Its only flag is `--no-restart`; anything else is rejected.

## 11.7 Send test calls — `calltrigger.sh`

Fires synthetic `/call` events at a running daemon so you can drive a call lifecycle
by hand, no phone system needed. It mirrors the wire shapes exactly.

```sh
scripts/calltrigger.sh -<flags> <callid> [caller] [dialed] [options]
```

**Event flags** (combine in any order — they always fire in lifecycle order
`i → a → t → h`):

| Flag | Event | Needs |
|---|---|---|
| `i` | Incoming Call | callid + caller + dialed |
| `a` | Accepted Call | callid + caller + dialed (+ optional `-u <user>`) |
| `t` | Transfer Call | callid + `-n <newuser>` (no phone numbers) |
| `h` | Hangup | callid + caller |

> The fifth shape, **Outgoing Call**, isn't covered by this tool — it only drives the
> inbound lifecycle. POST an outgoing payload directly instead (see
> [§2.3](02-integrating-call-api.md)).

**Positionals** (after the flag token): `<callid>` (required), `[caller]` → JSON
`remote`, `[dialed]` → JSON `dialed`.

**Options:**

| Option | Meaning | Default |
|---|---|---|
| `-c <num>` | caller number (overrides positional/default) | — |
| `-d <num>` | dialed number (overrides positional/default) | — |
| `-u <user>` | Accepted-call user handle (optional) | — |
| `-n <user>` | Transfer target handle (**required** for `-t`) | — |
| `-H <host>` | daemon host | `$AID_HOST` or `127.0.0.1` |
| `-P <port>` | daemon port | `$AID_PORT` or `8088` |
| `-s <sec>` | sleep between events (preserves per-callid ordering) | `0.2` |
| `-N` | dry run: print the JSON payloads, don't POST | off |
| `-v` | verbose `curl` | off |
| `--help` | print usage | — |

It prints each payload alongside the HTTP status, and warns you whenever a POST comes
back as something other than `202`.

**Examples:**

```sh
scripts/calltrigger.sh -i 1001 +4915112345678 +4930222              # one incoming
scripts/calltrigger.sh -iah 1001 +4915112345678 +4930222 -u alice   # full lifecycle, assigned
scripts/calltrigger.sh -t 1001 -n bob                               # transfer an existing call
scripts/calltrigger.sh -iah -N 1001 +49151 +49302 -u alice          # dry run: just print payloads
AID_HOST=192.168.178.54 scripts/calltrigger.sh -iath 1001 +49151 +49302 -n bob
```

## 11.8 Environment variables

| Variable | Used by | Meaning | Default |
|---|---|---|---|
| `AID_LIVE_E2E` | `ctest` (live test) | arms the forked-daemon E2E test | unset → test self-skips |
| `AID_DEPLOY_ROOT` | `deploy.sh` | deploy root mirroring the prod layout | `~/aid-dev` |
| `AID_HOST` | `calltrigger.sh` | default daemon host | `127.0.0.1` |
| `AID_PORT` | `calltrigger.sh` | default daemon port | `8088` |
| `AID_CALLER` | `calltrigger.sh` | default caller number | `+4915112345678` |
| `AID_DIALED` | `calltrigger.sh` | default dialed number | `+493020220000` |
| `ASAN_OPTIONS` / `UBSAN_OPTIONS` | test harness under `AID_SANITIZE` | sanitizer runtime flags | set per-test automatically |

## 11.9 Script reference

| Script | Args | Purpose |
|---|---|---|
| `build.sh` | `[cmake args]` | configure + build Debug into `build/` |
| `test.sh` | `[ctest args]` | build if needed, then run the suite via `ctest` |
| `sanitize.sh` | — | ASan + UBSan build in `build-asan/`, then run tests |
| `format.sh` | `[--check]` | `clang-format` the tree (in place, or check-only) |
| `build-ui.sh` | — | build the SvelteKit dashboard into `ui/build/` |
| `deploy.sh` | `[--no-restart]` | rebuild + install daemon and both plugins together |
| `calltrigger.sh` | `-<flags> <callid> [caller] [dialed] [opts]` | POST test `/call` events to a running daemon |

---

Next: [Troubleshooting & glossary →](12-troubleshooting-and-glossary.md)
