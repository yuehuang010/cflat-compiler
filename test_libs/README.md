# Real library interop tests

This suite checks CFlat header ingestion and a small set of runtime calls against real C/C++ libraries. It is separate from `test.sh` and the `Test/` suite. Tier 1 is required before landing interop changes; it is not part of the normal edit loop.

## Run

```text
./test_libs.sh [Release|Debug] [-t N] [-j N] [--warm] [--strict] [--include-disabled] [--list] [lib ...]
test_libs.bat [Release|Debug] [-t N] [-j N] [--warm] [--strict] [--include-disabled] [--list] [lib ...]
```

The default configuration is `CFLAT_CONFIG` or Release. Tier 1 and four workers are the defaults; the batch runner is sequential. `-t N` includes tiers 1 through N. Library names limit execution. `--strict` makes missing libraries fail instead of skip. `--include-disabled` tries cases carrying a `DISABLED` marker; failures are XFAIL and passes are XPASS, neither affecting the result. `--warm` repeats passing cases after the first pass. `--list` prints the selected cases without compiling. The runners use `out/libs/` for case work and `out/libs-cache/` for the compiler cache.

The Windows runner does not enforce the per-case `timeout` value: cmd.exe has no built-in way to time out a child process, and this runner currently executes cases synchronously. Its `-j` value is accepted for CLI parity but does not enable parallel execution. Windows cases run from a fresh case directory under `out/libs/`; logs are `compile.log` and `run.log`.

## Library configuration

Each library directory has a plain `lib.cfg`, with one `key=value` per line. Lines beginning with `#` are comments; do not add spaces around `=` or quotes. Values may contain space-separated paths.

- `tier=1|2|3` selects the test cadence.
- `root_mac=deps` uses `${CFLAT_VCPKG_INSTALLED:-$HOME/.cflat-compiler-deps/vcpkg_installed}/arm64-osx`; `root_mac=brew:<formula>` uses `brew --prefix <formula>`. `root_win=deps` uses `%CFLAT_VCPKG_INSTALLED%` (or `%USERPROFILE%\.cflat-compiler-deps\vcpkg_installed`) plus `x64-windows-static`; `root_win=env:<VAR>` uses that environment variable, and an unset variable skips the library. `root_win=testlibs` uses `test_libs/vcpkg_installed/x64-windows`, installed from `test_libs/vcpkg.json`; `CFLAT_TESTLIB_<LIBNAME>` (or the variable named by `env_win=<VAR>`) overrides that root when set. `root_mac=none` marks a library that has no macOS root (it skips).
- `probe=<relative path>` must exist below the root or the library is skipped (or failed with `--strict`).
- `include=<dirs>` lists include directories below the root. `lib_mac=<files>` and `lib_win=<files>` list libraries below it.
- `runpath_mac=<dir>` and `runpath_win=<dir>` add a runtime library directory when needed.
- `version_mac=<command>` and `version_win=<command>` print the installed library version. `hint_mac=<text>` and `hint_win=<text>` explain how to install a missing library.
- `args=<args>` are passed to the built case when it runs; `@REPO@` expands to the repository root.
- `timeout=<seconds>` records the per-case timeout; the current Windows runner does not enforce it.

`test_libs.bat` installs `test_libs/vcpkg.json` once (fmt, eigen3, curl, openblas, sdl3, sqlite3, zlib) before the first library that uses `root_win=testlibs`, by having cflat check one `package-vcpkg` case; a failed install is reported as FAIL with the log tail (`out/libs/vcpkg-install.log`). The `curl`, `openblas`, `sdl3`, `sqlite3`, and `zlib` cases use `import package-vcpkg`, so their `lib.cfg` sets only the root, probe, and runtime path. libtorch is the manifest feature `torch` (source build, hours) and is not installed by default.

On Windows, json and simdjson use the shared vcpkg dependencies at `%USERPROFILE%\.cflat-compiler-deps\vcpkg_installed\x64-windows-static` by default (override with `CFLAT_VCPKG_INSTALLED`). fmt and eigen use the vcpkg tree above unless `CFLAT_TESTLIB_FMT` / `CFLAT_TESTLIB_EIGEN` point at another installation root; torch uses `CFLAT_TESTLIB_LIBTORCH` or the `torch` feature's tree. Install hints appear when a root or probe is unavailable.

## Cases

Each `.cb` case begins with comment markers before code:

```cflat
// GUARDS: the interop behavior this case checks
// FROM: scratch spike sources consolidated into this case
// MODE: check
```

`GUARDS` and `FROM` are required. `MODE: check` compiles without linking or running; cases without a mode compile to an executable and run it. A `DISABLED` marker lists one or more existing repository paths that explain why the case is not enabled:

```cflat
// DISABLED: internal/issue/p3/example.md internal/plan/example.md
```

Keep each case independent. It must assert each value it prints, check all legs without early returns, print a clear failing leg, and return zero only when all checks pass. Use `i64` for C++ template arguments representing CFlat 64-bit integers. Add a new case by placing a numbered `<lib>_NN_<topic>.cb` file in its library directory and documenting its behavior in the header markers. `lib.cfg` controls where its library headers and binaries are found.

When a fix lands for a disabled case, remove its `DISABLED` marker in the same change. The case should then pass normally; stale marker paths are reported as failures.

## Tiers

| Tier | Cadence | Libraries |
| --- | --- | --- |
| 1 | Smoke: required before landing, runs in `buildci.sh` / `buildci.bat`; not in the dev loop | json, fmt, simdjson, and the Windows-only vcpkg libraries curl, openblas, sdl3, sqlite3, zlib |
| 2 | On request (`-t 2`); cadence not decided yet | eigen |
| 3 | On request (`-t 3`); giant, minutes per case | torch |
