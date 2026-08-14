# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

cflat is a C-dialect compiler targeting LLVM IR. It compiles CFlat (.cb files) - an extended C language with modern features - generics, interfaces, namespaces, operator overloading, ownership/lifetime, and null-safe access - to LLVM Intermediate Representation for native execution.

## Git
Do not commit to git.  Stash is allowed.  
Keep `master` linear: every commit on `master` must have a single parent.  Do not create merge commits (no merge commits on `master`); integrate work by rebasing so history stays linear.

## Dependencies
Do not modify the root `./vcpkg.json` without explicit permission.

## Scratch Directory

Use the repo-root `scratch/` folder for ALL temporary files - throwaway `.cb` repros, scratch scripts, intermediate outputs, compiled binaries used for one-off checks. Do not use the system temp directory or the session scratchpad; keeping temp files in `scratch/` makes them easy to inspect and clean up. `scratch/` is gitignored, and files there are never picked up by `test.bat` / `example.bat` wildcards.

## Agent Delegation: Cost vs Intelligence

**Codex Luna is the default implementation agent for ALL delegated work.** It replaces the
former `sonnet` / `opus` tier split - there is no tier choice to make any more, for mechanical
work or for hard compiler work. The main session plans, coordinates, and reviews the results.

This section addresses the MAIN SESSION only. A spawned implementation agent does the work
itself in its own session; it must never re-delegate to another agent - if it believes the task
is wrong for it, it should say so in its report instead of spawning a sub-agent.

| Tier | Relative cost | Right for |
|------|---------------|-----------|
| Codex Luna (`gpt-5.6-luna`, High effort) | Low | **Default for everything.** High intelligence at low cost, so there is no reason to trade down: mechanical work (renames, builds/tests, doc updates, regression tests) and hard compiler work alike (multi-file changes across grammar + ForwardRefScanner + codegen, debugging with unclear root cause, ownership/lifetime work) |
| `opus` (`opus-general-purpose`) | High | Fallback only: Codex unavailable, or the task genuinely needs the Claude toolchain (skills, worktree tooling) |
| `sonnet` (`general-purpose-sonnet`) | Low | Not used - Codex Luna covers this range at comparable cost and higher intelligence |

Codex Luna is not an Agent-tool subagent - it is the `codex` CLI (`~/.local/bin/codex`), run
non-interactively from Bash. `~/.codex/config.toml` already pins `model = "gpt-5.6-luna"` and
`model_reasoning_effort = "high"`, so no flags are needed for the model or effort:

```bash
codex exec --sandbox workspace-write -C /Users/felixhuang/source/cflat-compiler "<self-contained prompt>"
```

Add `-m` / `-c model_reasoning_effort=...` only to override those defaults. The prompt must be
self-contained - Codex does not see this session's context.

Guidelines:

- Delegate implementation to Codex Luna by default. Fall back to a Claude `opus` Agent-tool
  subagent only when Codex is unavailable or the task genuinely needs the Claude toolchain
  (skills, worktree tooling); never to `sonnet` or `haiku`.
- Default flow: main session writes the plan and acceptance criteria, runs Codex Luna, then
  verifies the result (build + the current host's suite: `test.bat` on Windows, `./test.sh` on
  macOS/Linux).
- Give the implementation agent a self-contained prompt: exact files, the plan, constraints from
  this file (both-pass ParseDeclarationSpecifiers, LogError-only, ASCII, no new test files), and
  how to verify.
- Use the read-only `Explore` agent for broad codebase searches instead of burning main-session context.
- If Codex Luna fails or flails, re-run with the failure context added, or escalate to an `opus`
  subagent - do not re-run the same prompt unchanged, and do not silently absorb the work into
  the main session for convenience.
- Independent sub-tasks should be run in parallel when they touch disjoint files.

## Token Efficiency

Tokens cost real money and energy. Treat context as a budget, not a scratch pad. The goal is
the same answer for fewer tokens - not a worse answer.

- **Read narrowly.** Use `Grep`/`Glob` to find the exact spot, then `Read` with `offset`/`limit`.
  Try not to re-read a file you just edited to confirm the edit - `Edit` fails loudly if it did not apply.
- **Delegate bulk reading.** Broad "where is X handled" sweeps go to the read-only `Explore`
  agent, which returns the conclusion instead of dumping files into the main session. See
  [Agent Delegation](#agent-delegation-cost-vs-intelligence) for tier choice.
- **Cap command output.** Pipe noisy commands through `Select-Object -Last N` / `-First N`, or
  redirect to a file under `scratch/` and read only the tail. Never dump a whole build log.
- **Batch independent work.** Issue independent tool calls in a single message so they run in
  parallel; do not serialize them into one-per-turn round trips.
- **Do not re-derive.** Facts already established earlier in the conversation stand. Do not
  re-run a passing suite, re-confirm a decision, or restate the plan each turn.
- **Write tersely.** No preamble, no recap of what the user just said, no summary table when a
  sentence does. Skip options you are not going to take.
- **Bisect, do not brute-force.** When narrowing a failure, binary-search the input rather than
  reading everything; verify the probe is actually reproducing the failure before trusting a
  run of "passes" (a probe that fails for an unrelated reason makes the search vacuous).

## Skills

Project skills are **authored and tracked in `internal/skill/<name>/SKILL.md`** (tracked in
git, like `internal/plan/` and `internal/issue/`). `.gitignore` ignores all of `.claude/`,
so a skill living only under `.claude/skills/` is invisible to git and lost on a fresh
clone - that is why the source of truth is `internal/skill/`.

**Claude Code only auto-discovers project skills under `.claude/skills/`**, so a skill in
`internal/skill/` is NOT invocable as `/<name>` - verified: with nothing under
`.claude/skills/`, a fresh session reports no project skills. `internal/skill/` is the
tracked, reviewable home; it is deliberately not the live one, and no symlink bridges them.

Two ways to use a skill from here:

- **Read it directly.** The file is a plain procedure - point at
  `internal/skill/<name>/SKILL.md` and follow it. This is the default and needs no setup.
- **Activate it for a session**, if you want the `/<name>` slash command:

  ```bash
  mkdir -p .claude/skills/<name>
  cp internal/skill/<name>/SKILL.md .claude/skills/<name>/SKILL.md   # restart the session to pick it up
  ```

  That copy is gitignored and can drift. `internal/skill/` stays the source of truth: edit
  there, re-copy, and never edit the copy under `.claude/`.

Current skills:

| Skill | Purpose |
|-------|---------|
| `fix-issue` | Fix an `internal/issue/` entry in an isolated worktree with a delegated agent, review with an opus code-review agent until clean, then merge to `master` as a single-parent commit. This skill is the one place the "do not commit" rule is lifted. |

## Text Output

- Use plain ASCII characters for readable text in source files, comments, log messages, and documentation. Avoid Unicode punctuation such as en/em dashes, smart quotes, and ellipsis characters. Use ASCII `-`, `"`, `'`, and three dots `...` instead.

## Comments

- Keep inline comments to 2 lines or fewer. Multiline comments are allowed above a function or at the beginning entry into a new scope.

## Logging Conventions

- Use `LogError` or `LogErrorContext` for all error reporting in the compiler. Do not introduce `LogWarning` or leave `std::cout`.

## Debugging Workflow

- Before editing, state the hypothesized root cause and verify against the codebase. Avoid speculative edits based on a single guess (e.g., do not assume lexer token conflicts before checking parser/grammar paths).

- After finding the root cause of the issue, consider writing a regression test.
- When encountering a LLVM assert, after identifying the root cause, then write an proper error message in the compiler to avoid that case.
- Known, diagnosed-but-deferred bugs/gaps live in `internal/issue/` (tracked in git, like `internal/plan/`). Check there before re-investigating a failure, and record new known issues there (one file per issue: summary, repro, root cause, fix direction). Delete the file when the issue is fixed. `internal/issue/` holds ACTIVE items only (one file per issue under `p1/`,`p2/`, `p3/`, `ui/`); the landed design records (ratified behaviour changes and approaches that must not be retried) live in the digest at the bottom of `internal/fix-issue-lessons.md`.
- Durable lessons from past fix rounds - review sequencing, guard polarity, what to distrust in an agent report, how tests go vacuous - live in `internal/fix-issue-lessons.md`. Read it before starting a non-trivial compiler fix; add to it when a lesson changes an outcome twice.

## Building

The Windows build uses **CMake + vcpkg (Ninja + MSVC)** - this is the default path, and what the dev scripts (`buildAndRun.bat`, `buildci.bat`) invoke. vcpkg supplies the dependencies (ANTLR4, LLVM); the build also deploys `core/*.cb` next to the exe. See [Cross-platform builds](#cross-platform-builds-cmake-windows-linuxwsl-macos) below for the `cmake_build.bat` helper and the presets. The CMake build writes `cflat.exe` to the `x64/<Config>/` layout that `test.bat` / `test_lsp.bat` expect.

**Quick dev loop** - `buildAndRun.bat` builds Debug + Release (via `cmake_build.bat`), then runs `Test/test_basic.cb`:

```bash
./buildAndRun.bat            # builds Debug + Release, runs test_basic.cb
./buildAndRun.bat test_foo.cb  # same but runs test_foo.cb instead
```

### Cross-platform builds (CMake: Windows, Linux/WSL, macOS)

**CMake + vcpkg is the build system.** It is the only path, and the only one that works on Linux/WSL and macOS. Presets live in `CMakePresets.json`. See `internal/macos-build.md` for the macOS internals. Working end-to-end: Windows + Linux/WSL host build, Linux ELF target, and macOS arm64 native build + link + run on Apple Silicon (`./test.sh` passes 178/0 in both Debug and Release; run it with Homebrew tools on PATH).

> The CMake build writes the Windows `cflat.exe` to the `x64/<Config>/` layout, so `test.bat` / `test_lsp.bat` work unchanged after a CMake build.

**Windows (Ninja + MSVC).** Use the helper - it runs `vcvars64`, sets `VCPKG_ROOT`, and resolves the shared dependency tree that lives **outside the source dir** at `%USERPROFILE%\.cflat-compiler-deps\vcpkg_installed` (override with `CFLAT_VCPKG_INSTALLED`; no rebuild). On a fresh clone, populate it once with `vcpkg-build.bat`:

```bash
./cmake_build.bat release    # builds win-x64-release -> x64/Release/cflat.exe
./cmake_build.bat debug      # builds win-x64-debug   -> x64/Debug/cflat.exe
```

Equivalent manual invocation (from a dev shell with `VCPKG_ROOT` set and `vcvars64` sourced):

```bash
cmake --preset win-x64-release && cmake --build --preset win-x64-release
```

**Linux / WSL (Ninja + apt clang/llvm-18).** Source of truth stays on `/mnt/c`; build artifacts go to the native fs (`~/cflat-build`). The Linux preset is standalone (no vcpkg toolchain) and points at apt's `/usr/lib/llvm-18` + the antlr 4.10 jar at `/opt/antlr`. One-time toolchain (Ubuntu-24.04): `apt install clang llvm-18-dev cmake ninja-build default-jre nlohmann-json3-dev libsimdjson-dev libantlr4-runtime-dev uuid-dev` plus the antlr 4.10.1 jar in `/opt/antlr`.

From inside WSL:

```bash
cd /mnt/c/source/cflat-compiler
cmake --preset linux-x64-release && cmake --build --preset linux-x64-release
# binary -> ~/cflat-build/linux-x64-release/cflat
```

Driving the Linux build from the Windows host:

```bash
wsl.exe -e bash -lc "cd /mnt/c/source/cflat-compiler && cmake --preset linux-x64-release && cmake --build --preset linux-x64-release"
```

> antlr versions **must** match the runtime: Linux pins generator 4.10.1 against libantlr4-runtime 4.10 (Windows uses 4.13.2). Don't cross them.

**macOS arm64 native build (validated on Apple Silicon).** Build with Homebrew tools + `openjdk` on PATH:

```bash
./cmake_build.sh release   # resolves VCPKG_ROOT + openjdk PATH, then configures/builds
./test.sh Release          # 554 passed, 0 failed, 8 skipped
```

`cmake_build.sh` is the Mac/Linux counterpart to `cmake_build.bat` (`debug` | `release`, picks the preset from `uname`). The macOS preset points `VCPKG_INSTALLED_DIR` at a **shared tree outside the source dir** (`~/.cflat-compiler-deps/vcpkg_installed`, override with `CFLAT_VCPKG_INSTALLED`) - see [Git worktrees](#git-worktrees) below. The build is **self-contained (no Xcode / Command Line Tools)** after a one-time `cflat --init` (bundled `ld64.lld` + a harvested libSystem tbd stub). Toolchain, link path, self-contained mechanism, Darwin runtime specifics (`if const (__MACOS__)`), and `--run` on Mach-O are documented in [`internal/macos-build.md`](internal/macos-build.md).

### Git worktrees

`vcpkg_installed` is 12 GB (macOS) / ~26 GB (Windows) and rarely changes. On **both**
platforms it lives *outside* the source tree in a fixed per-user location - macOS
`~/.cflat-compiler-deps/vcpkg_installed`, Windows `%USERPROFILE%\.cflat-compiler-deps\vcpkg_installed`
(override either with `CFLAT_VCPKG_INSTALLED`) - so a plain `git worktree add` just
works with zero post-processing, no junction, no symlink, and no deletion hazard:

```bash
git worktree add ../cflat-feature -b feature/foo
cd ../cflat-feature && ./cmake_build.sh release   # or cmake_build.bat on Windows; no vcpkg step
cd ../cflat-feature && x64/Release/cflat --init-local   # per-worktree compiler cache
git worktree remove ../cflat-feature               # shared tree untouched
```

**In a worktree, use `--init-local`, not `--init`.** The compiler cache is *not* shared the
way `vcpkg_installed` is: `--init` writes one per-user `~/.cflat` / `%USERPROFILE%\.cflat`
that every worktree and every build config would fight over, so a `--init` in one worktree
silently replaces the core bitcode another worktree is about to compile against.
`--init-local` puts the cache in `<exe dir>/.cflat` - i.e. `x64/Release/.cflat` inside that
worktree - and every later compile from that same exe picks it up automatically, no flag
needed. This is the preferred collision-avoidance mechanism for worktrees, and for Debug vs
Release side by side. `test.sh`, `test.bat`, and `example.bat` already run `--init-local`
for exactly this reason, so a suite run never clobbers your per-user cache.
`git worktree remove` takes the local cache with it; nothing to clean up separately.

`cmake_build.sh` resolves `VCPKG_ROOT` from the main checkout's gitignored `./vcpkg`
clone (a linked worktree has none); `cmake_build.bat` does the equivalent via
`VCPKG_ROOT`/`vcvars64`. Both scripts wipe a build dir whose cached `VCPKG_INSTALLED_DIR`
no longer matches, since a moved dependency tree leaves stale absolute paths (`LLVM_DIR`
etc.) behind. See [`internal/worktree-vcpkg-sharing.md`](internal/worktree-vcpkg-sharing.md)
for the full mechanism and how to populate the shared tree on a fresh clone.

**What forces a 50-min LLVM source rebuild** (on any platform): vcpkg keys its binary cache (`~/.cache/vcpkg/archives`, user-global) on an ABI hash whose inputs are the port version (registry baseline in `vcpkg-configuration.json`), the **feature list in `vcpkg.json`**, the triplet, the host compiler, and the **CMake version**. The source directory is not an input - a worktree can never cause a rebuild. Editing `vcpkg.json` features, bumping the baseline, or `brew upgrade cmake`/Xcode can.

## Running

```bash
# Compile to native executable
x64/Debug/cflat.exe input.cb -o out.exe

# Also dump LLVM IR
x64/Debug/cflat.exe input.cb -o out.exe --out-lli out.ll

# Execute IR directly via lli
x64/Debug/cflat.exe input.cb --out-lli out.ll && lli.exe out.ll
```

The compiler automatically locates `runtime.cb` next to the executable. The CFlat source uses the `.cb` extension.

### In-process execution (`--run`)

```bash
# JIT-compile and run in-process - no exe on disk; exit code is the program's
x64/Debug/cflat.exe input.cb --run

# Pass arguments to the program (everything after `--` becomes argv[1..])
x64/Debug/cflat.exe input.cb --run -- arg1 arg2
```

Entry must be `int main()` or `int main(int argc, char** argv)`. Read-only: cannot be combined with `-o`, `-l/--out-lli`, or `-b/--bitcode`, and single-threaded only (programs that spawn a `thread<T>` or use the `program` construct are rejected - compile to an exe instead). See [`doc/CLI.md`](doc/CLI.md) for the full command-line reference.

### Compiler cache (`--init`)

Run once after installing or updating cflat to populate `%USERPROFILE%\.cflat\`:

```bash
x64/Debug/cflat.exe --init
```

Pre-compiles the core `.cb` libraries to LLVM bitcode and caches resolved linker paths, so subsequent compiles load bitcode instead of re-parsing (~44% faster cold start). The cache is keyed on the core `.cb` mod-times and auto-invalidates.

`--init-local` populates `<exe dir>/.cflat` instead of the per-user cache; once created, later compiles from that same exe pick it up automatically (no flag needed) - useful for portable installs, CI, or keeping several builds/worktrees from sharing one cache. `--init-clear` deletes **both** the per-user and local caches; `--init-clear-local` deletes only the local one. A `CFLAT_CACHE_DIR` env var override and the full 4-step resolution order are documented in [`doc/CACHING.md`](doc/CACHING.md), which also covers the full design and troubleshooting.

### C interop (`.c` files compiled by clang)

`.c` inputs are treated as **real C** and compiled by `clang-cl` into objects that are merged into the final image by `lld-link`. They are NOT parsed by the CFlat parser.

**Auto-extern**: when a `.c` is brought in, the compiler auto-registers every externally-linkable function via clang's JSON AST dump - `import "util.c";` works without hand-written prototypes. Hand-written `extern` declarations take precedence.

Two ways to bring a C file in (both require `-o`):

```bash
x64/Debug/cflat.exe app.cb util.c -o app.exe   # positional input
# or: import "util.c"; inside a .cb
```

### Binding a prebuilt C library (header + import lib)

```bash
x64/Debug/cflat.exe app.cb --c-include <inc-dir> --c-lib <path/to/lib.lib> --c-define CURL_STATICLIB -o app.exe
```

- **Source**: `import package "curl/curl.h" lib "libcurl.lib";` - `.h`/`.hpp`/`.hh` extension routes to header binding. Use `lib { "a.lib", "b.lib" }` for multi-lib. Use `define "NAME"` for per-import defines. Use `cache` clause for large headers (e.g. `import "windows.h" cache;`).
- **Non-self-contained headers**: use grouped import `import {"windows.h", "tlhelp32.h"};` when a header assumes another was included first.
- **CLI**: `--c-include <dir>`, `--c-lib <path>`, `--c-define NAME[=val]`, `--c-header-cache-deep`.

See `internal/c-interop-anon-records.md` for struct/union detail. See [`doc/CLI.md`](doc/CLI.md) for full flag list.

> Note: `.c` means real C. CFlat test fixtures use `.cb` (e.g. `Test/test_c.cb`).

### Key CLI flags

See [`doc/CLI.md`](doc/CLI.md) for the full reference. Most-used flags:

- `-o / --output`: Output native executable (.exe)
- `-l / --out-lli`: Output LLVM IR file (.ll)
- `-g / --debug-info`: Emit DWARF debug information
- `-i / --import-dir`: Directory to search for imported modules
- `-v / --verbose`: Print detailed diagnostic messages
- `-ftime-trace`: Write Chrome-trace JSON to `<input>.time-trace.json`
- `--symbol <name>`: IDE-style quick symbol lookup, then exit (repeatable)
- `--check`: Check source files for errors without emitting output
- `--run`: JIT-compile and run in-process

## Testing
- **Windows**: `test.bat` / `test_lsp.bat` / `example.bat` (batch scripts).
- **Linux/WSL and macOS**: `test.sh` is the `test.bat` counterpart - it compiles+runs the platform-portable subset of `Test/*.cb` (plus `Test/errors/*.cb`) against the native cflat, in parallel with a per-test timeout, and prints a PASS/FAIL/SKIP summary. Run it as `bash test.sh Release` (or `Debug`, `-j N`). It maintains an explicit SKIP list of genuinely Windows-only tests; before adding one, prove the *whole file* is Windows-bound. The SKIP-list rationale and the warm-cache second pass are documented in [`internal/testing-notes.md`](internal/testing-notes.md).
- **`--init` serializer rule** (load-bearing): `--init` reconstructs compiler state from a hand-written serializer, so any new field on `TypeAndValue` / `StructData` / `AnnotationValue` that an analysis reads MUST be added to the `LLVMBackend.cpp` cache round-trip in the same change - otherwise it is silently dropped on a warm cache and `expect_error` tests stop firing. See `internal/testing-notes.md`.
- **Verify on the host you are on, and only that host.** On a macOS/Linux host, a green `./test.sh` is the bar for declaring work complete; on Windows it is `test.bat` (Release). Run the suite for the current host after compiler changes, before saying you are done.
- **Do NOT append "but this still needs verification on Windows/WSL" caveats.** The maintainer owns cross-platform verification and is already aware that a macOS session cannot run `test.bat`. Repeating it every turn is noise. Just report what you ran and what passed. Flag a *specific*, concrete cross-platform risk only when you have an actual reason to believe a change behaves differently on another platform (e.g. an ifdef'd path you could not exercise) - not as a routine disclaimer.
- `test.bat` runs all tests in parallel and should complete in under a minute. A test that hangs will be killed after a configurable timeout (default 120 seconds, set via `TIMEOUT_SECS` at the top of `test.bat`).
- Do NOT create separate compiler integration tests - test.bat already validates the compiler end-to-end.
- Do NOT create new test files (e.g. in `Test/`) unless explicitly instructed to. Add regression cases by extending an existing, related test file instead.
- Do NOT revert changes to check if baseline is correct.  Assume all tests are passing and failed test are from the current changes.  Ask before reverting changes to validate baseline.
- When tests fail after a fix, investigate root causes; do not weaken/dilute test assertions to make them pass. Ask before disabling tests.

## Running Tests

```bash
test.bat              # runs against Release (default)
test.bat Debug        # runs against Debug
test.bat Release      # explicit Release
```

> **Performance tip**: Building Release and running `test.bat` (Release) is significantly faster than `test.bat Debug`. Prefer Release for the full test loop; use Debug only when you need symbols for a specific failure.

`test.bat` defaults to Release. Pass `Debug` or `Release` as the first argument to override. The `CFLAT_CONFIG` environment variable is also respected (command-line arg takes precedence).

> **Pitfall**: If `CFLAT_CONFIG` is set in the shell that invokes `test.bat`, it will be used as the default unless a command-line arg overrides it. After rebuilding with a different configuration, clear or update `CFLAT_CONFIG` so tests run against the intended binary.

### LSP Tests

Run the LSP test suite (smoke tests + fixture/scenario tests) with:

```bash
test_lsp.bat          # runs against Release (default)
test_lsp.bat Debug    # runs against Debug
```

LSP tests live in `vscode-extension/test/`. After any change to `LspServer.cpp`, `LspSymbolIndex.cpp`, or `MainListener.h` (symbol registration), run `test_lsp.bat` to verify LSP behaviour. These are kept separate from `test.bat`.

### Example Programs

Build all example programs in `example/` and subdirectories:

```bash
example.bat           # runs against Release (default)
example.bat Debug     # runs against Debug
```

`example.bat` compiles all runnable `.cb` files in the `example/` tree, automatically skipping library/helper files (`threadpool.cb`, `test_helper.cb`, internal network modules) and setting appropriate import paths per category. Reports pass/fail/skip counts; exits with code 1 if any example fails to compile.

To compile a single example manually:

```bash
x64/Debug/cflat.exe example/bitmap.cb -o out/bitmap.exe
```

To run a single test manually:

```bash
x64/Debug/cflat.exe Test/test_operators.cb -i Test/library -o out/test_operators.exe --out-lli out/test_operators.ll
out\test_operators.exe
```

Current tests (all in `Test/`, all CFlat with `-i Test\library`) (`test_helper.cb` is a shared helper, not run directly.)

test.bat and test.sh glob `Test/test_*.cb` plus `Test/errors/err_*.cb` (non-recursive) to locate tests; `Test/library/` and other prefixes are never picked up. Remember to remove debug tests matching those prefixes or they would be picked up by the wildcard expansion.

### Error tests

Negative tests live in `Test/errors/` and use the `expect_error` compiler built-in. `test.bat` compiles each `err_*.cb` and expects exit code 0. To run one individually:

```bash
x64/Debug/cflat.exe Test/errors/err_missing_return.cb -i Test/library
# prints: PASS: expected error received
# exit code: 0
```

Two forms are supported:

```cflat
// Bare-semicolon form - error must occur before the enclosing scope closes
extern int main()
{
    expect_error("Undefined variable foo.");
    int x = foo + 1;
}

// Scoped block form (statement scope) - error must occur inside the braces
expect_error("nullable '?' is not allowed on primitive type 'int'") {
    int? x = 0;
}

// Scoped block form (file scope) - for testing function/struct definitions
expect_error("missing a return statement") {
    int compute(int x) { int y = x * 2; }
}

// Bare-semicolon form at file scope - covers IMPORT-TIME errors (raised during
// ProcessImports, before the listener walk): the expectation is armed before imports run.
expect_error("does not compile on its own");
import "tlhelp32.h";
```

The substring in `expect_error` is matched against the error message text (not the `file(line,col):` prefix). The compiler exits 0 on match, 1 with a diagnostic on mismatch or if the block compiles without error.

To add a new error test: create `Test/errors/err_<description>.cb`. No script changes needed.

## Architecture

### Compilation Pipeline

```
Source (.cb) -> CFlatLexer/CFlatParser (ANTLR4) -> Parse Tree
    -> ForwardRefScanner (pre-pass)
    -> MainListener (code generation)
    -> LLVM Module -> .ll / .bc -> native .exe
```

### Two-Pass Compilation

1. **ForwardRefScanner** (`MainListener.h`): Pre-registers struct shells, function signatures, and generic instantiations before codegen. Enables forward references and monomorphizes generics (`Box<int>` -> symbol `Box__int`, double-underscore mangling). Also detects `move` parameters and `if const` blocks.
2. **MainListener** (`MainListener.h`): Walks the AST and emits LLVM IR using `LLVMBackend` as the backend.

Both passes share `ParseDeclarationSpecifiers()` - any change to type parsing must be applied in **both** the `ForwardRefScanner` copy and the main `MainListener` copy.

### Core Components

| File | Role |
|------|------|
| `CFlat.g4` | ANTLR4 grammar defining CFlat syntax |
| `LLVMBackend.h/.cpp` | Compiler engine: type system, symbol tables, LLVM IR generation |
| `MainListener.h` | AST visitor implementing both ForwardRefScanner and codegen passes |
| `CompilerManager.h` | Singleton crash handler - installs CRT assert hook, SIGABRT handler, and LLVM fatal error handler; dumps compiler state on any assert/crash |
| `ArgParser.h` | CLI argument parsing |
| `main.cpp` | Entry point |
| `LspServer.h/.cpp` | Language Server Protocol server (hover, completion, go-to-definition) |
| `LspSymbolIndex.h/.cpp` | LSP symbol index built during compilation |
| `JsonRpcLoop.h/.cpp` | JSON-RPC protocol loop for LSP communication |
| `LspTypes.h` | LSP type definitions |
| `core/` | Standard library - compiled alongside every program |

### Key Internal State (in `LLVMBackend`)

- `stackNamedVariable`: Deque of scopes tracking local variables per block/function
- `globalNamedVariable`: Global variable map
- `dataStructures`: Struct type registry (fields, destructor, interface VTables)
- `functionTable`: Function overload registry and resolution
- `interfaceTable`: Interface method contracts
- `stringPool`: Interned string literals
- `returnBlockTable`: Inlined return-block function bodies
- `builder / module / context`: LLVM IR generation state
- `diBuilder`: DWARF debug info builder (active with `-g`)
- `parseTreeCache_`: timestamp-validated cache of parsed ANTLR trees for **implicit core-library imports only** (files under `runtimeDir/core`). Reused across compiles and LSP re-analyses since core content is stable; deliberately **not** cleared by `ResetForReanalysis`. User imports are parsed fresh into `importedParseStates` (per-compile, cleared on reset). `ResetForReanalysis` must clear *all* transient per-call state (e.g. `lastCallIsBonded`) or a value left set by an aborted compile leaks into the next file's analysis.

`NamedVariable` has an `IsOwning` flag; `TypeAndValue` has an `IsMove` flag - both drive the ownership/lifetime system.

### Language Features

See `internal/language-features.md` for the full feature list (generics, interfaces, arrays, module system, brace-init, ownership/move, compile-time features).

Quick reference: `doc/LANGUAGE.md` is the user-facing language reference.

### Adding New Language Features

| Goal | Where to change |
|------|----------------|
| New syntax | Edit `CFlat.g4`; rebuild triggers ANTLR regeneration |
| New type or IR operation | Add methods to `LLVMBackend.h` |
| New statement or expression | Add `Parse*()` / `exit*()` handler in `MainListener.h` |
| Forward-declare a new construct | Add scan logic to `ForwardRefScanner` in `MainListener.h` |
| New binary operator | `TryBinaryOperatorOverload()` in `MainListener.h` + `Operation` enum in `LLVMBackend.h` |
| New soft keyword (like `move`) | Text-match in both `ParseDeclarationSpecifiers()` copies in `MainListener.h` - do NOT add to the ANTLR lexer |
| New grammar keyword statement | Add rule to `CFlat.g4`; add `ctx->newRule()` retrieval in `ParseStatement`; add no-op overrides in `ForwardRefScanner` if the rule can appear at file scope |

### Debugging Compiler Crashes

- `CompilerManager.h` installs crash handlers that dump compiler state on assert/abort
- Rerun with `-v` to see detailed diagnostics
- Check `.ll` output (`--out-lli`) to inspect LLVM IR
- LLVM assertion `"Ptr must have pointer type"` usually means `GetType()` was called without `allowPointer=true` for a pointer parameter

### Standard Library

Only `runtime.cb` is auto-imported. All other core libraries require an explicit `import "filename.cb"`.

See `internal/stdlib-reference.md` for the full table of all core/*.cb files and their exports.

To add a new core library: add the `.cb` file to `cflat/core/`. CMake's `CONFIGURE_DEPENDS` glob copies the whole `core/` directory to the exe dir automatically, so a rebuild picks it up - no project file entry needed.

Use `nullptr` instead of `null`. Always assign `default` to fields.

### VS Code Extension

```bash
cd vscode-extension
build.bat    # compile
install.bat  # install into VS Code
```

Reload VS Code to activate syntax highlighting for `.cb` files.

## Performance Benchmarking

Always use `performance.bat` with `-O2`. See `internal/performance-benchmarks.md` for benchmark files, reference throughput numbers, and stream/channel design notes.
