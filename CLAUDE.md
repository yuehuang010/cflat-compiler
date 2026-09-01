# CLAUDE.md

Guidance for Claude Code in this repo. Terse style is deliberate - condensed, not sloppy.
Every line is a rule or a fact. Grammar is clipped to save tokens; meaning is not.

## Overview

cflat = C-dialect compiler -> LLVM IR. Source `.cb`. Extended C: generics, interfaces,
namespaces, operator overloading, ownership/lifetime, null-safe access.

## Git

Do not commit. Stash ok. `master` stays linear - single parent per commit, no merge commits,
integrate by rebase. (`fix-issue` skill is the one exception; see Skills.)

## Dependencies

Do not touch root `./vcpkg.json` without explicit permission.

## Scratch Directory

ALL temp files -> repo-root `scratch/`: throwaway `.cb` repros, scripts, intermediate output,
one-off binaries. Not system temp, not session scratchpad. Gitignored; never picked up by
`test.bat` / `test_example.bat` wildcards.

## Agent Delegation

**Codex Luna = default implementation agent for ALL delegated work.** No tier choice - mechanical
work and hard compiler work alike. Main session plans, coordinates, reviews.

Applies to MAIN SESSION only. A spawned agent does the work itself; it must NEVER re-delegate.
Wrong task for it -> say so in the report, do not spawn a sub-agent.

Codex Luna is not an Agent-tool subagent. It is the `codex` CLI (`~/.local/bin/codex`), run from
Bash. `~/.codex/config.toml` already pins `model = "gpt-5.6-luna"` + `model_reasoning_effort =
"high"` - no flags needed:

```bash
codex exec --sandbox workspace-write -C /Users/felixhuang/source/cflat-compiler "<self-contained prompt>"
```

`-m` / `-c model_reasoning_effort=...` only to override. Prompt must be self-contained - Codex
does not see this session.

- Fall back to Claude `opus` Agent-tool subagent (`opus-general-purpose`) ONLY if Codex
  unavailable, or task needs Claude toolchain (skills, worktree tooling). Never `sonnet`, never
  `haiku`.
- Flow: main session writes plan + acceptance criteria -> run Codex -> main session verifies
  (build + current host suite).
- Prompt must carry: exact files, plan, constraints from this file (both-pass
  ParseDeclarationSpecifiers, LogError-only, ASCII, no new test files), how to verify.
- Broad "where is X handled" searches -> read-only `Explore` agent, not main context.
- Codex fails/flails -> re-run WITH failure context added, or escalate to `opus`. Never re-run
  same prompt unchanged. Never silently absorb the work into main session.
- Independent sub-tasks touching disjoint files -> run parallel.

## Token Efficiency

Tokens cost money and energy. Same answer, fewer tokens - not a worse answer.

- **Read narrow.** `Grep`/`Glob` to find spot, then `Read` with `offset`/`limit`. Do not re-read a
  file you just edited - `Edit` fails loudly if it did not apply.
- **Delegate bulk reading** to `Explore`. It returns the conclusion, not the files.
- **Cap output.** Pipe through `Select-Object -Last N` / `-First N`, or redirect to `scratch/` and
  read the tail. Never dump a whole build log.
- **Batch.** Independent tool calls in one message. No one-per-turn round trips.
- **No re-derive.** Established facts stand. Do not re-run a passing suite, re-confirm a decision,
  restate the plan each turn.
- **Write tersely.** No preamble, no recap, no summary table where a sentence does. Skip options
  you will not take.
- **Bisect, do not brute-force.** Binary-search the input. Verify the probe actually reproduces the
  failure first - a probe failing for an unrelated reason makes the whole search vacuous.

## Skills

Authored + tracked in `internal/skill/<name>/SKILL.md` (git-tracked, like `internal/plan/` and
`internal/issue/`). `.gitignore` ignores all of `.claude/`, so a skill living only there is
invisible to git and dies on fresh clone.

Claude Code auto-discovers project skills ONLY under `.claude/skills/`. So `internal/skill/` is
NOT invocable as `/<name>` - verified. It is the tracked home, deliberately not the live one. No
symlink bridges them.

Two uses:
- **Read it directly** - plain procedure, point at `internal/skill/<name>/SKILL.md`, follow it.
  Default, no setup.
- **Activate for a session** (want the `/<name>` command):
  ```bash
  mkdir -p .claude/skills/<name>
  cp internal/skill/<name>/SKILL.md .claude/skills/<name>/SKILL.md   # restart session to pick up
  ```
  That copy is gitignored and drifts. `internal/skill/` stays source of truth - edit there,
  re-copy, never edit the copy.

| Skill | Purpose |
|-------|---------|
| `fix-issue` | Fix an `internal/issue/` entry in an isolated worktree via delegated agent, review with opus code-review agent until clean, merge to `master` as single-parent commit. Only place the do-not-commit rule is lifted. |

## Text Output

Plain ASCII in source, comments, log messages, docs. No Unicode punctuation (no en/em dash, smart
quotes, ellipsis char). Use `-`, `"`, `'`, three dots.

## Comments

Inline comments <= 2 lines. Multiline allowed above a function, or at entry to a new scope.

## Logging Conventions

`LogError` / `LogErrorContext` for ALL compiler error reporting. Never add `LogWarning`. Never
leave `std::cout`.

## Localization

**Never edit `cflat/locales/` directly** - not by hand, not by sed/script, not from an agent.
Covers every file there (de/en/es/fr/it/ja/ko/ru/zh-Hans/zh-Hant, en-pseudo). They are
GENERATED: `en-pseudo.json` comes from the `LogError*` format strings in code via build/test
tooling; translations are maintained externally from it. New/changed diagnostic -> write ONLY the
`LogError*` format string in code, let scripts/tests regenerate, commit regenerated result as-is.
A `cflat/locales/` diff not from the generator = review defect.

## Debugging Workflow

- State hypothesized root cause + verify against codebase BEFORE editing. No speculative edits off
  one guess (e.g. do not blame lexer token conflicts before checking parser/grammar paths).
- Root cause found -> consider a regression test.
- Hit an LLVM assert -> after root-causing, add a proper compiler error message to prevent that case.
- Read `internal/fix-issue-lessons.md` before any non-trivial compiler fix: review sequencing, guard
  polarity, what to distrust in an agent report, how tests go vacuous. Add to it when a lesson
  changes an outcome twice. Landed design records (ratified changes, approaches never to retry)
  live in the digest at the bottom of that file.

**`internal/issue/` = ACTIVE items only**, one file per issue (summary, repro, root cause, fix
direction). Check before re-investigating a failure; file new known issues there; delete the file
on fix. Buckets: `p1/`-`p3/` bugs+gaps by severity, `p4/` small quality-of-life FEATURES (language
or library conveniences too small for `internal/plan/`, e.g. `arr.length()` on a fixed array),
`ui/`. A `p4/` file records proposed spelling, alternatives, acceptance - and needs a maintainer
ruling on the surface before build. Never file a bug under `p4/`. Never start a `p4/` item without
the ruling.

## Building

### Fresh clone (Windows): `bootstrap.bat`

One command, clean checkout -> verified Release build: toolchain check (VS/vcvars64, cmake, ninja,
git, antlr4) -> vcpkg deps -> clone + source-build + install LLVM into
`%USERPROFILE%\.cflat-compiler-deps\` -> `cmake_build.bat release` -> `cflat --init-local` ->
`test.bat Release`. Every step idempotent, re-running is cheap. Narrow with `/skip-llvm`,
`/skip-tests`, `/llvm-only`, `/fresh`. LLVM ninja build tree is deleted after install succeeds
(`/keep-build` keeps it for incremental re-bump); install tree + source clone stay, shared by every
worktree.

LLVM source build is mandatory and slow - no RTTI-enabled LLVM prebuilt exists for Windows, vcpkg's
port is far behind.

**cflat pins one exact LLVM version, no version shims** - a different LLVM will not compile. Pinned
version lives in `CMakePresets.json`; read it there, not here. `bootstrap.bat` provisions BOTH
installs: `/MT` assertions-OFF tree (Release links this), and `/MTd` CRT
`LLVM_ENABLE_ASSERTIONS=ON` tree with `-assert` suffix (Debug presets point here). So a fresh clone
builds LLVM twice. Recipe + the two-install rationale:
[`internal/llvm-from-source-build.md`](internal/llvm-from-source-build.md).

**Debug links assertions-enabled LLVM on purpose.** Release LLVM has `LLVM_ENABLE_ASSERTIONS=OFF`,
so LLVM APIs that assert on misuse silently return garbage instead - a `getTerminator()` behaviour
change is the worked example. Debug is the only config where that safety net exists.
Keep both installs in sync when bumping.

### Shared dependency tree (all platforms)

`vcpkg_installed` (small since LLVM left the manifest) + source-built LLVM install (large) rarely
change, so both live OUTSIDE the source tree at a fixed per-user location:
`~/.cflat-compiler-deps/` (macOS/Linux), `%USERPROFILE%\.cflat-compiler-deps\` (Windows). Override
the vcpkg tree with `CFLAT_VCPKG_INSTALLED`. Consequence: plain `git worktree add` just works - no
junction, no symlink, no deletion hazard, no vcpkg step per worktree.

Populate on a fresh clone with `vcpkg-build.bat` (Windows). Full mechanism:
[`internal/worktree-vcpkg-sharing.md`](internal/worktree-vcpkg-sharing.md).

**What forces a full LLVM source rebuild** (any platform): vcpkg keys its binary cache
(`~/.cache/vcpkg/archives`, user-global) on an ABI hash over port version (registry baseline in
`vcpkg-configuration.json`), **feature list in `vcpkg.json`**, triplet, host compiler, **CMake
version**. Source dir is NOT an input - a worktree can never cause a rebuild. Editing `vcpkg.json`
features, bumping the baseline, or `brew upgrade cmake`/Xcode can.

### Per-platform build

CMake + vcpkg is the build system - only path, and the only one working on Linux/WSL and macOS.
Presets in `CMakePresets.json`. vcpkg supplies ANTLR4 / nlohmann-json / simdjson (LLVM is separate,
source-built). Build also deploys `core/*.cb` next to the exe. Windows output goes to the
`x64/<Config>/` layout `test.bat` / `test_lsp.bat` expect.

**Windows (Ninja + MSVC).** Default path; `buildAndRun.bat` / `buildci.bat` invoke it. Helper runs
`vcvars64`, sets `VCPKG_ROOT`, resolves the shared tree:

```bash
./cmake_build.bat release    # -> x64/Release/cflat.exe
./cmake_build.bat debug      # -> x64/Debug/cflat.exe
cmake --preset win-x64-release && cmake --build --preset win-x64-release   # manual equivalent
```

Quick dev loop - builds Debug + Release, then runs a test:

```bash
./buildAndRun.bat              # runs Test/test_basic.cb
./buildAndRun.bat test_foo.cb  # runs that instead
```

**macOS arm64 (validated on Apple Silicon).** Homebrew tools + `openjdk` on PATH.
`cmake_build.sh` = counterpart to `cmake_build.bat` (`debug` | `release`, picks preset from
`uname`), resolves `VCPKG_ROOT` + openjdk PATH:

```bash
./cmake_build.sh release
./test.sh Release
```

Self-contained - no Xcode / Command Line Tools - after one-time `cflat --init` (bundled `ld64.lld`
+ harvested libSystem tbd stub). Toolchain, link path, self-contained mechanism, Darwin specifics
(`if const (__MACOS__)`), `--run` on Mach-O: [`internal/macos-build.md`](internal/macos-build.md).

**Linux / WSL (Ninja + apt clang/llvm).** Source stays on `/mnt/c`; artifacts to native fs
(`~/cflat-build`). Preset is standalone (no vcpkg toolchain), points at apt LLVM under `/usr/lib/`
+ antlr jar at `/opt/antlr`. One-time toolchain (Ubuntu-24.04): `apt install clang cmake
ninja-build default-jre nlohmann-json3-dev libsimdjson-dev libantlr4-runtime-dev uuid-dev` plus
matching `llvm-<N>-dev` and the antlr 4.10.1 jar in `/opt/antlr`.

```bash
cd /mnt/c/source/cflat-compiler
cmake --preset linux-x64-release && cmake --build --preset linux-x64-release
# -> ~/cflat-build/linux-x64-release/cflat
# from Windows host: wsl.exe -e bash -lc "cd /mnt/c/source/cflat-compiler && cmake --preset linux-x64-release && cmake --build --preset linux-x64-release"
```

> **Linux preset is STALE.** Its `CMAKE_PREFIX_PATH` points at an apt LLVM several majors behind the
> pinned version, and the compat shims that bridged that gap were removed - this path does not build
> today. Fix the preset before trusting this subsection.

> antlr generator and runtime versions MUST match: Linux pins generator 4.10.1 against
> libantlr4-runtime 4.10; Windows uses 4.13.2. Do not cross them.

### Git worktrees

Shared dep tree (above) means no post-processing:

```bash
git worktree add ../cflat-feature -b feature/foo
cd ../cflat-feature && ./cmake_build.sh release        # or cmake_build.bat; no vcpkg step
cd ../cflat-feature && x64/Release/cflat --init-local  # per-worktree compiler cache
git worktree remove ../cflat-feature                   # shared tree untouched
```

**In a worktree use `--init-local`, NOT `--init`.** Compiler cache is NOT shared the way
`vcpkg_installed` is: `--init` writes one per-user `~/.cflat` / `%USERPROFILE%\.cflat` that every
worktree and config fights over, so `--init` in one worktree silently replaces core bitcode another
is about to compile against. `--init-local` -> `<exe dir>/.cflat` (i.e. `x64/Release/.cflat`), and
every later compile from that exe picks it up automatically, no flag. Preferred collision-avoidance
for worktrees AND for Debug vs Release side by side. `test.sh`, `test.bat`, `test_example.bat` already
run `--init-local`, so a suite run never clobbers your per-user cache. `git worktree remove` takes
the local cache with it.

`cmake_build.sh` resolves `VCPKG_ROOT` from the main checkout's gitignored `./vcpkg` clone (a linked
worktree has none); `cmake_build.bat` does it via `VCPKG_ROOT`/`vcvars64`. Both wipe a build dir
whose cached `VCPKG_INSTALLED_DIR` no longer matches - a moved dep tree leaves stale absolute paths
(`LLVM_DIR` etc.) behind.

## Running

```bash
x64/Debug/cflat.exe input.cb -o out.exe                        # native exe
x64/Debug/cflat.exe input.cb -o out.exe --out-lli out.ll       # also dump IR
x64/Debug/cflat.exe input.cb --out-lli out.ll && lli.exe out.ll  # run IR via lli
```

Compiler auto-locates `runtime.cb` next to the exe. Source extension is `.cb`.

### In-process execution (`--run`)

```bash
x64/Debug/cflat.exe input.cb --run                # no exe on disk; exit code is the program's
x64/Debug/cflat.exe input.cb --run -- arg1 arg2   # everything after -- becomes argv[1..]
```

Entry must be `int main()` or `int main(int argc, char** argv)`. Read-only: cannot combine with
`-o`, `-l/--out-lli`, `-b/--bitcode`. Single-threaded only - programs spawning `thread<T>` or using
`program` are rejected, compile to an exe instead.

### Compiler cache (`--init`)

```bash
x64/Debug/cflat.exe --init        # populate per-user %USERPROFILE%\.cflat\
```

Pre-compiles core `.cb` libs to LLVM bitcode + caches resolved linker paths, so later compiles load
bitcode instead of re-parsing - substantial cold-start win. Keyed on core `.cb` mod-times,
auto-invalidates.

`--init-local` -> `<exe dir>/.cflat` instead (portable installs, CI, several builds/worktrees not
sharing one cache; see Git worktrees). `--init-clear` deletes BOTH caches; `--init-clear-local`
only the local one. `CFLAT_CACHE_DIR` override + full 4-step resolution order + design +
troubleshooting: [`doc/CACHING.md`](doc/CACHING.md).

### C interop (`.c` files compiled by clang)

`.c` inputs = REAL C. Compiled by `clang-cl` into objects, merged into the final image by
`lld-link`. NOT parsed by the CFlat parser. (CFlat test fixtures use `.cb`, e.g. `Test/test_c.cb`.)

**Auto-extern**: bringing in a `.c` auto-registers every externally-linkable function via clang's
JSON AST dump, so `import "util.c";` works with no hand-written prototypes. Hand-written `extern`
declarations take precedence.

Two ways in, both require `-o`:

```bash
x64/Debug/cflat.exe app.cb util.c -o app.exe   # positional input
# or: import "util.c"; inside a .cb
```

### Binding a prebuilt C library (header + import lib)

```bash
x64/Debug/cflat.exe app.cb --c-include <inc-dir> --c-lib <path/to/lib.lib> --c-define CURL_STATICLIB -o app.exe
```

- **Source form**: `import package "curl/curl.h" lib "libcurl.lib";` - `.h`/`.hpp`/`.hh` routes to
  header binding. `lib { "a.lib", "b.lib" }` for multi-lib. `define "NAME"` for per-import defines.
  `cache` clause for large headers (`import "windows.h" cache;`).
- **Non-self-contained headers**: grouped import `import {"windows.h", "tlhelp32.h"};` when a header
  assumes another was included first.
- **CLI**: `--c-include <dir>`, `--c-lib <path>`, `--c-define NAME[=val]`, `--c-header-cache-deep`.

Struct/union detail: `internal/c-interop-anon-records.md`.

### Key CLI flags

Full reference: [`doc/CLI.md`](doc/CLI.md). Most-used:

- `-o / --output`: native executable
- `-l / --out-lli`: LLVM IR file (.ll)
- `-g / --debug-info`: DWARF debug info
- `-i / --import-dir`: import search dir
- `-v / --verbose`: detailed diagnostics
- `-ftime-trace`: Chrome-trace JSON -> `<input>.time-trace.json`
- `--symbol <name>`: IDE-style quick symbol lookup then exit (repeatable)
- `--check`: check for errors, emit nothing
- `--run`: JIT-compile and run in-process

**`--symbol-dump` family** - dump ONE section of output instead of the whole thing, then exit. All
three are repeatable and need a positional source file. Use these to inspect a single function
instead of diffing a whole `--out-lli` dump.

| Switch | Dumps |
|--------|-------|
| `--symbol-dump` | symbol info for source elements |
| `--symbol-dump-ir` | unoptimized LLVM IR |
| `--symbol-dump-opt` | optimized LLVM IR; defaults to `-O2`, explicit `-O0`/`-O1`/`-O2` overrides |

Selectors: `line:<n>`, `line:<a>-<b>` (`--symbol-dump` only), `function:<name>`, and `module` (IR
dumps only). No line ranges for IR dumps; an IR line selector resolves to the function enclosing
that 1-based line.

```bash
cflat probe.cb --symbol-dump function:main
cflat probe.cb --symbol-dump-opt module
```

Multiple positional files -> analyzed in order, output split by a file banner; the optimized view
reuses the previous file's snapshot where applicable (`CFLAT_VIEW_NO_INCREMENTAL=1` disables that,
useful when comparing). Line dumps skip comments and string/char literals; unresolved identifiers
yield no detail. Function dumps read the file holding the indexed definition, imports included.

## Testing

Scripts by host - Windows `.bat`, macOS/Linux `.sh`:

```bash
test.bat            # compiler suite, Release default; also: test.bat Debug | Release
test_lsp.bat        # LSP suite, Release default; also: test_lsp.bat Debug
test_example.bat         # example programs, Release default; also: test_example.bat Debug
bash test.sh Release   # test.bat counterpart (also Debug, -j N)
./test_example.sh           # test_example.bat counterpart
```

Config: first arg wins; `CFLAT_CONFIG` env var is respected otherwise. **Pitfall**: a stale
`CFLAT_CONFIG` in the invoking shell silently becomes the default - clear/update it after
rebuilding a different config. Prefer Release for the full loop (much faster than Debug); use Debug
only when you need symbols for a specific failure.

`test.sh` compiles+runs the platform-portable subset of `Test/*.cb` plus `Test/errors/*.cb` against
native cflat, parallel, per-test timeout, PASS/FAIL/SKIP summary. It keeps an explicit SKIP list of
genuinely Windows-only tests - before adding one, prove the WHOLE file is Windows-bound. SKIP-list
rationale + warm-cache second pass: [`internal/testing-notes.md`](internal/testing-notes.md).

`test.bat` runs parallel and should finish quickly; a hung test is killed after `TIMEOUT_SECS` (top
of `test.bat`, default 120s).

`test_example.bat` compiles all runnable `.cb` in `example/`, skips library/helper files
(`threadpool.cb`, `test_helper.cb`, internal network modules), sets import paths per category,
exits 1 if any example fails.

Run `test_lsp.bat` after ANY change to `LspServer.cpp`, `LspSymbolIndex.cpp`, or `MainListener.h`
(symbol registration). LSP tests live in `vscode-extension/test/`, kept separate from `test.bat`.

### Rules

- **Verify on the host you are on, and only that host.** macOS/Linux -> green `./test.sh` is the
  bar. Windows -> `test.bat` Release. Run it after compiler changes before saying you are done.
- **No "still needs verification on Windows/WSL" caveats.** Maintainer owns cross-platform
  verification and knows a macOS session cannot run `test.bat`. Report what you ran and what
  passed. Flag a cross-platform risk only when SPECIFIC and concrete (e.g. an ifdef'd path you
  could not exercise), never as routine disclaimer.
- **`--init` serializer rule (load-bearing).** `--init` rebuilds compiler state from a hand-written
  serializer. Any new field on `TypeAndValue` / `StructData` / `AnnotationValue` that an analysis
  reads MUST be added to the `LLVMBackend.cpp` cache round-trip in the SAME change - else it is
  silently dropped on a warm cache and `expect_error` tests stop firing. See
  `internal/testing-notes.md`.
- Do NOT create separate compiler integration tests - `test.bat` already covers end-to-end.
- Do NOT create new test files (e.g. in `Test/`) unless told to. Extend an existing related file.
- Do NOT revert changes to check the baseline. Assume tests passed before; failures come from the
  current change. Ask first if you really need a baseline check.
- Tests fail after a fix -> find root cause. Never weaken/dilute assertions to go green. Ask before
  disabling a test.

### Manual single runs

```bash
x64/Debug/cflat.exe Test/test_operators.cb -i Test/library -o out/test_operators.exe --out-lli out/test_operators.ll
out\test_operators.exe
x64/Debug/cflat.exe example/bitmap.cb -o out/bitmap.exe
```

Test discovery: `test.bat`/`test.sh` glob `Test/test_*.cb` + `Test/errors/err_*.cb`, non-recursive.
`Test/library/` and other prefixes are never picked up (`test_helper.cb` is a shared helper, not run
directly). Remove debug files matching those prefixes or the wildcard grabs them.

### Error tests

Negative tests in `Test/errors/`, using the `expect_error` built-in. Each `err_*.cb` must compile
with exit code 0. New test = new `Test/errors/err_<description>.cb`, no script changes.

```bash
x64/Debug/cflat.exe Test/errors/err_missing_return.cb -i Test/library
# prints: PASS: expected error received   (exit 0)
```

Forms:

```cflat
// Bare semicolon - error must occur before the enclosing scope closes
extern int main()
{
    expect_error("Undefined variable foo.");
    int x = foo + 1;
}

// Scoped block, statement scope - error must occur inside the braces
expect_error("nullable '?' is not allowed on primitive type 'int'") {
    int? x = 0;
}

// Scoped block, file scope - for function/struct definitions
expect_error("missing a return statement") {
    int compute(int x) { int y = x * 2; }
}

// Bare semicolon at file scope - covers IMPORT-TIME errors (raised during ProcessImports,
// before the listener walk): expectation is armed before imports run.
expect_error("does not compile on its own");
import "tlhelp32.h";
```

Substring matches the error message TEXT, not the `file(line,col):` prefix. Exit 0 on match; exit 1
with a diagnostic on mismatch or if the block compiles clean.

## Architecture

```
Source (.cb) -> CFlatLexer/CFlatParser (ANTLR4) -> Parse Tree
    -> ForwardRefScanner (pre-pass)
    -> MainListener (code generation)
    -> LLVM Module -> .ll / .bc -> native .exe
```

### Two-pass compilation

1. **ForwardRefScanner** (`MainListener.h`): pre-registers struct shells, function signatures,
   generic instantiations before codegen. Enables forward refs; monomorphizes generics (`Box<int>`
   -> `Box__int`, double-underscore mangling). Also detects `move` params and `if const` blocks.
2. **MainListener** (`MainListener.h`): walks the AST, emits LLVM IR via `LLVMBackend`.

Both share `ParseDeclarationSpecifiers()` - **any type-parsing change must be applied to BOTH
copies** (the `ForwardRefScanner` one and the `MainListener` one).

### Core components

| File | Role |
|------|------|
| `CFlat.g4` | ANTLR4 grammar |
| `LLVMBackend.h/.cpp` | Engine: type system, symbol tables, IR generation |
| `MainListener.h` | AST visitor - both ForwardRefScanner and codegen passes |
| `CompilerManager.h` | Singleton crash handler: CRT assert hook, SIGABRT, LLVM fatal error handler; dumps compiler state on assert/crash |
| `ArgParser.h` | CLI parsing |
| `main.cpp` | Entry point |
| `LspServer.h/.cpp` | LSP server (hover, completion, go-to-definition) |
| `LspSymbolIndex.h/.cpp` | Symbol index built during compilation |
| `JsonRpcLoop.h/.cpp` | JSON-RPC loop for LSP |
| `LspTypes.h` | LSP type definitions |
| `core/` | Standard library, compiled alongside every program |

### Key internal state (`LLVMBackend`)

- `stackNamedVariable`: deque of scopes, locals per block/function
- `globalNamedVariable`: global variable map
- `dataStructures`: struct registry (fields, destructor, interface VTables)
- `functionTable`: overload registry + resolution
- `interfaceTable`: interface method contracts
- `stringPool`: interned string literals
- `returnBlockTable`: inlined return-block function bodies
- `builder / module / context`: IR generation state
- `diBuilder`: DWARF builder (active with `-g`)
- `parseTreeCache_`: timestamp-validated cache of parsed ANTLR trees for **implicit core-library
  imports only** (`runtimeDir/core`). Reused across compiles and LSP re-analyses since core content
  is stable; deliberately NOT cleared by `ResetForReanalysis`. User imports parse fresh into
  `importedParseStates` (per-compile, cleared on reset). `ResetForReanalysis` must clear ALL
  transient per-call state (e.g. `lastCallIsBonded`) or a value left by an aborted compile leaks
  into the next file's analysis.

`NamedVariable` has `IsOwning`; `TypeAndValue` has `IsMove`. Both drive ownership/lifetime.

### Language features

Full list (generics, interfaces, arrays, module system, brace-init, ownership/move, compile-time):
`internal/language-features.md`. User-facing reference: `doc/LANGUAGE.md`.

### Adding new language features

| Goal | Where |
|------|-------|
| New syntax | `CFlat.g4`; rebuild regenerates ANTLR |
| New type or IR operation | methods on `LLVMBackend.h` |
| New statement or expression | `Parse*()` / `exit*()` handler in `MainListener.h` |
| Forward-declare a new construct | scan logic in `ForwardRefScanner` (`MainListener.h`) |
| New binary operator | `TryBinaryOperatorOverload()` in `MainListener.h` + `Operation` enum in `LLVMBackend.h` |
| New soft keyword (like `move`) | text-match in BOTH `ParseDeclarationSpecifiers()` copies - do NOT add to the ANTLR lexer |
| New grammar keyword statement | rule in `CFlat.g4` + `ctx->newRule()` retrieval in `ParseStatement` + no-op overrides in `ForwardRefScanner` if it can appear at file scope |

### Debugging compiler crashes

- `CompilerManager.h` handlers dump compiler state on assert/abort
- `-v` for detailed diagnostics
- `--out-lli` to inspect the IR
- LLVM assert `"Ptr must have pointer type"` usually = `GetType()` called without
  `allowPointer=true` for a pointer parameter

### Standard library

Only `runtime.cb` is auto-imported; everything else needs explicit `import "filename.cb"`. Full
table of `core/*.cb` and exports: `internal/stdlib-reference.md`.

New core library = drop the `.cb` into `cflat/core/`. CMake `CONFIGURE_DEPENDS` glob copies the
whole dir next to the exe, so a rebuild picks it up - no project file entry.

Use `nullptr`, not `null`. Always assign `default` to fields.

### VS Code extension

```bash
cd vscode-extension
build.bat    # compile
install.bat  # install into VS Code
```

Reload VS Code to activate `.cb` syntax highlighting.

## Performance Benchmarking

Always `performance.bat` with `-O2`. Benchmark files, reference throughput, stream/channel design
notes: `internal/performance-benchmarks.md`.
