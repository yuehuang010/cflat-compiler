# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

cflat is a C-dialect compiler targeting LLVM IR. It compiles CFlat (.cb files) - an extended C language with modern features - generics, interfaces, namespaces, operator overloading, ownership/lifetime, and null-safe access - to LLVM Intermediate Representation for native execution.

## Git
Do not commit to git.  Stash is allowed.  

## Dependencies
Do not modify the root `./vcpkg.json` without explicit permission.

## Text Output

- Use plain ASCII characters for readable text in source files, comments, log messages, and documentation. Avoid Unicode punctuation such as en/em dashes, smart quotes, and ellipsis characters. Use ASCII `-`, `"`, `'`, and three dots `...` instead.

## Logging Conventions

- Use `LogError` or `LogErrorContext` for all error reporting in the compiler. Do not introduce `LogWarning` or leave `std::cout`.

## Debugging Workflow

- Before editing, state the hypothesized root cause and verify against the codebase. Avoid speculative edits based on a single guess (e.g., do not assume lexer token conflicts before checking parser/grammar paths).

- After finding the root cause of the issue, consider writing a regression test.
- When encountering a LLVM assert, after identifying the root cause, then write an proper error message in the compiler to avoid that case.
- Known, diagnosed-but-deferred bugs/gaps live in `internal/issue/` (gitignored, like `internal/plan/`). Check there before re-investigating a failure, and record new known issues there (one file per issue: summary, repro, root cause, fix direction). Delete the file when the issue is fixed.

## Building

Visual Studio 2022 project with vcpkg dependencies (ANTLR4, LLVM) and deploy core/*.cb libraries.  Always build via the **solution file** - building the `.vcxproj` alone puts the exe in the wrong location for `test.bat`:

```bash
msbuild cflat.slnx -p:Configuration=Debug -p:Platform=x64
```

> **Bash / Git Bash note**: Use **`-p:`** (dash), not `/p:` (slash). Git Bash path-converts arguments that start with `/letter:`, stripping the leading slash, which causes MSBuild to misparse the flags. Dashes are safe.

Full msbuild path (if not on PATH):

```bash
"/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/amd64/MSBuild.exe" cflat.slnx -p:Configuration=Debug -p:Platform=x64 -v:minimal
```

**Quick dev loop** - `buildAndRun.bat` builds both Release and Debug, then runs the Debug binary on `Test/test_basic.cb`. Run directly (without `cmd /c`) so MSBuild output is captured:

```bash
./buildAndRun.bat            # builds Release + Debug, runs Debug binary on test_basic.cb
./buildAndRun.bat test_foo.cb  # same but runs test_foo.cb instead
```

### Git worktrees (share the 26 GB vcpkg_installed)

`vcpkg_installed` is ~26 GB / ~50 min to build. Use these repo-root scripts so worktrees share the main checkout's copy via a junction instead of rebuilding:

```bash
new-worktree.bat C:\source\cflat-feature -b feature/foo   # add worktree + junction, no vcpkg rebuild
rm-worktree.bat  C:\source\cflat-feature                  # safe remove (unlinks junction first)
```

- **Never `rm -rf` or Explorer-delete a worktree** - that can follow the junction and wipe the shared 26 GB. Always use `rm-worktree.bat`.
- See [`internal/worktree-vcpkg-sharing.md`](internal/worktree-vcpkg-sharing.md) for the mechanism, the fresh-mtime rebuild trap, and the optional snapshot fallback.

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

This pre-compiles all 20 core `.cb` libraries to LLVM bitcode and caches the resolved linker paths (MSVC lib dirs, Windows SDK dirs). Subsequent compiles load the bitcode instead of re-parsing, cutting cold-start time by ~44% (~820ms -> ~460ms for the parse+codegen phase). The cache is keyed on the modification times of the core `.cb` files and auto-invalidates when any of them change. See [`doc/CACHING.md`](doc/CACHING.md) for the full design and troubleshooting.

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
- Always run `test.bat` after compiler changes to verify all tests pass before declaring work complete.
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

test.bat uses wildcard `Test/*.*` to locate tests. Remember to remove debug tests or else they would be picked up by the wildcard expansion.

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

To add a new core library: add the `.cb` file to `core/` and add an entry in `cflat.vcxproj` with `DeploymentContent`.

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
