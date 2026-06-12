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

`.c` inputs are treated as **real C** and compiled by `clang-cl` into objects that are merged into the final image by `lld-link` (see `CompileCFile` and `EmitExecutable` in `LLVMBackend.h`). They are NOT parsed by the CFlat parser. clang-cl auto-detects the Windows SDK / MSVC, so no extra flags are needed; the compiled object supplies the definitions.

**Auto-extern (no manual declarations needed)**: when a `.c` is brought in, the compiler runs clang's JSON AST dump (`ExtractCSignatures` in `LLVMBackend.h`) and auto-registers every externally-linkable function the `.c` *defines* as if you had written an `extern` for it - so `import "util.c";` works without hand-written prototypes. The signatures also feed the LSP symbol index, giving hover / completion / go-to-definition (jumps into the `.c`) for the C functions. Hand-written `extern` declarations still work and take precedence (first-writer-wins). Auto-extern covers the same scalar+pointer subset the extern ABI supports (`int`, sized ints, `float`/`double`, `bool`, pointers incl. opaque struct/`void*`). Struct/union *by value* is supported (see `EmitAbiLoweredCall`); 3+ levels of pointer indirection collapse to opaque `void**` since CFlat's `TypeAndValue` exposes at most two pointer levels (run with `-v` to see what was collapsed or skipped). C `long` maps to 32-bit (Windows LLP64), `long long` to 64-bit.

> Extraction results are cached process-wide (`cFileSigCache_`), keyed on the canonical `.c` path: the timestamp is checked first and the FNV-1a content hash only when the timestamp differs, so the LSP does not re-spawn clang on every keystroke. The clang spawn is serialized (`cClangSpawnMutex_`) and gated out of object emission in LSP mode (`symbolSink_`). LSP analysis runs on a worker pool inside `CrashRecoveryContext`; because a spawned child inherits handles, the worker's temp-file removal uses the non-throwing `std::filesystem::remove` overload and `WorkerLoop` wraps each analysis in try/catch so no analysis can take the server down. The server also clears `HANDLE_FLAG_INHERIT` on its JSON-RPC pipes (`LspServer.cpp`).

**Toolchain deployment**: `clang-cl.exe` and `lld-link.exe` are copied next to `cflat.exe` by the `CopyLldDependencies` target in `cflat.vcxproj`. `FindClangCl()` prefers that deployed copy (resolved via `runtimeDir`, the exe's own directory) before falling back to `PATH`, so C interop is self-contained and needs nothing on `PATH`.

Two ways to bring a C file in (both require `-o`, since linking is what merges the object):

```bash
# As an extra positional input
x64/Debug/cflat.exe app.cb util.c -o app.exe

# Or via import from inside a .cb (resolved through the normal import search dirs)
#   import "util.c";
x64/Debug/cflat.exe app.cb -o app.exe
```

> Note: `.c` means real C. CFlat test fixtures that were historically given a `.c` extension now use `.cb` (e.g. `Test/test_c.cb`, `Test/library/stringlib.cb`, `Test/library/testmodule.cb`).

### Binding a prebuilt C library (header + import lib)

Unlike a `.c` (which cflat *compiles*), a prebuilt library (libcurl, zlib, ...) is consumed by extracting its **header** and linking its **import lib**. Obtain both via Conan/vcpkg; cflat consumes the paths.

- **Source declares what to bind**: `import package "curl/curl.h";` (or a bare `import "curl/curl.h";` - a `.h/.hpp/.hh` extension routes to the same path). The header is resolved against the `--c-include` roots. An optional inline `lib` clause names the import library to link: `import package "curl/curl.h" lib "libcurl.lib";` (works on the bare `import "x.h"` form too; applies to that line only). Use the brace form `lib { "a.lib", "b.lib" }` when one header needs several libs (e.g. `import "windows.h" lib { "user32.lib", "gdi32.lib" };` for a Win32 GUI app). Lib resolution: a library beside the importing `.cb` is linked by path; a bare system lib name not found there (e.g. `user32.lib`) is passed through to lld-link, which finds it on the SDK lib dir cflat discovered for the header - so no `--c-lib` / absolute path is needed (`ResolveCLinkLib` in `LLVMBackend.h`). One or more inline `define` clauses add per-import preprocessor defines, scoped to that header's AST dump and appended on top of the CLI `--c-define`: `import package "curl/curl.h" lib "libcurl.lib" define "CURL_STATICLIB" define "FOO=1";` (clauses come after `lib`).
- **CLI supplies where the files are** (machine-specific, so they stay out of source): repeatable `--c-include <dir>` (header search roots, also used as `-I` for the AST dump), `--c-lib <path>` (import library added to the `lld-link` line), and `--c-define NAME[=val]` (a preprocessor `/D` applied to *all* clang-cl invocations - the header dump, the `.c` auto-extern dump, and the `.c` object compile).

```bash
x64/Debug/cflat.exe app.cb --c-include <inc-dir> --c-lib <path/to/lib.lib> --c-define CURL_STATICLIB -o app.exe
```

> The inline `lib` and `define` clauses apply only to their own import line; `--c-define` (CLI) applies process-wide to every clang-cl spawn, and per-import `define` clauses are appended after it (so the same header bound twice with different inline defines does not collide on a stale signature cache entry). `lib`, `define`, and `cache` are soft keywords (inline grammar literals, same mechanism as `program`).

**Opt-in disk cache for large headers (`cache` clause)**: a trailing `cache` clause persists the extracted decls of that header to `%USERPROFILE%\.cflat\cheaders\<key>.json`, so the next *cold* compile loads the JSON instead of re-running the clang header parse (the dominant cost for big headers like `windows.h`, ~1.3s). It is opt-in per import: `import "windows.h" cache;` or `import package "curl/curl.h" lib "libcurl.lib" cache;` (the clause comes last, after any `lib`/`define`). The cache key folds the canonical header path, every `--c-include` dir, every `--c-define`, and every inline `define` - so a header exposed differently under different roots/defines never collides. By default validation is **shallow**: the top header's mtime/hash plus the version-stamped SDK include dirs baked into the key (an SDK upgrade changes those paths -> automatic miss). The CLI switch `--c-header-cache-deep` upgrades cached headers to **transitive** validation, recording and re-checking every `#include`d file's mtime/hash (catches an in-place SDK header edit the top-header check would miss, at the cost of validating hundreds of files). This is distinct from the `import package-vcpkg` cache, which is co-located in `vcpkg_installed/.cflat-cache/`. See [`doc/CACHING.md`](doc/CACHING.md).

`CompileCHeader` (`LLVMBackend.h`) AST-dumps a stub that `#include`s the header and registers the externally-linkable function **declarations**, **enum constants** (bare globals, matching C's flat enum scope - `CURLOPT_URL`, not `Type.CURLOPT_URL`), **struct/union types** (a `typedef struct Tag {...} Name;` is usable under either `Tag` or `Name` - the typedef is registered as a type alias), and **object-like `#define` constants** that fold to an integer/float/string value (`WM_DESTROY`, `WS_OVERLAPPEDWINDOW`, ...; a macro that does not fold to a constant is skipped). A source-location filter keeps only functions / enum constants / macros under the `--c-include` roots / the header's dir, so the transitively-included SDK/CRT API surface is excluded; **structs are an exception** - the transitive by-value dependency closure of the in-scope structs is also registered (so `MSG`, which embeds a `POINT` from the SDK `shared/` dir, sizes correctly) - and so is the **Windows SDK `shared/` sibling dir** (a header bound from `um/` also folds `shared/` constants: `ERROR_*`, `MAX_PATH`, HRESULTs). A folded integer macro keeps its natural C type (`((DWORD)-10)` -> `u32`, not a width-guessed `i64`), and a pseudo-handle cast macro (`HKEY_LOCAL_MACHINE`) folds to a `void*` constant. A C function-pointer struct field (e.g. `WNDPROC lpfnWndProc`) is registered as a bare `void*`, since CFlat's 16-byte `function<T>` closure would corrupt the struct's ABI layout. A **fixed-size array struct field** (e.g. `CHAR szExeFile[260]`) is registered as an inline CFlat array (`RegisterCRecords` peels the `[N]` extents and sets `ConstArraySize` rather than letting the shared mapper decay it to a pointer), so the C ABI `sizeof` is exact - `sizeof(PROCESSENTRY32)` is 304, not the decayed-pointer 40 that silently dropped the array. Only positive integer extents become inline arrays; an incomplete `[]` still decays to a pointer, and a field whose element type is unmappable abandons the whole record (leaving an opaque shell that errors on use - never a silent size change). **Non-self-contained headers (grouped import)**: a header that uses `HANDLE`/`DWORD`/`WINAPI`/... without including the header that defines them (e.g. `tlhelp32.h`, which assumes `windows.h` was included first) does not compile on its own - clang drops every dependent function declaration (`unknown type 'HANDLE'`). The compiler has **no built-in knowledge** of any header (no windows.h special case); the dependency must be expressed in source via a **grouped import** `import {"windows.h", "tlhelp32.h"};`. The C-header entries of a group are extracted as **one translation unit** (`CompileCHeaderGroup` -> `ExtractCHeaderClang` takes the ordered header list; the stub `#include`s each in listed order), so an earlier entry satisfies a later one's prerequisites. Registration scope is the union of each group header's directory plus the `um/`<->`shared/` sibling expansion, so each header contributes only its own decls. A bare `import "tlhelp32.h";` fails loudly: `ExtractCHeaderClang` detects the `unknown type name` error (counted by `PrereqDiagConsumer` in `CClangExtract.cpp`, excluding the in-stub macro-probe errors via `isInMainFile`), registers nothing, and `ReportOrphanHeader` emits a diagnostic suggesting the grouped form. The ordered group list is folded into both the in-memory and the disk `cache` keys. Group-level `lib`/`define`/`cache` clauses apply to the whole group (`CompileImportGroup` in `LLVMBackend.cpp`). Variadics (e.g. `curl_easy_setopt`) are supported. Same scalar+pointer subset and per-header caching as the `.c` auto-extern path. After a successful link, a sibling runtime DLL of each `--c-lib` (in the lib's dir or a `../bin` sibling, the Conan layout) is auto-copied next to the exe.

**CLI flags** (see `ArgParser.h`):
- `-o / --output`: Output native executable path (.exe)
- `-l / --out-lli`: Output LLVM IR file path (.ll)
- `-b / --bitcode`: Output LLVM bitcode file (.bc)
- `-g / --debug-info`: Emit DWARF debug information
- `-i / --import-dir`: Directory to search for imported modules
- `-p / --platform`: Target platform - `x64` (default) or `x86`
- `-v / --verbose`: Print detailed diagnostic messages during compilation
- `-ftime-trace`: Write a Chrome-trace JSON of the compile to `<input>.time-trace.json` (or `check.time-trace.json` under `--check`). Matches clang's single-dash spelling. Load it in `chrome://tracing` or Perfetto; the dominant phases are `ProcessImports` / `CHeaderExtract` (libclang header parsing) and `RuntimeImport` (core-library parse). The double-dash `--ftime-trace` also parses, but the single-dash form is canonical.
- `--symbol <name>`: IDE-style quick symbol lookup, then exit (repeatable). An exact (or case-insensitive) name match prints detailed info - kind, signature, source location, doc comment, and, for a type/namespace/interface, every member with its signature; a miss falls back to substring / edit-distance matching and suggests the closest symbols. Reuses the same symbol index that drives LSP hover / go-to-definition (the canonical, never-stale API surface), giving an agent the discovery path a human gets from an editor. Indexes the positional source file if one is given (true IDE semantics - results reflect that file's imports); with no source file it synthesizes an import-all-core file so the whole standard library is searchable with zero setup. Implemented in `RunSymbolQuery` (`main.cpp`) on top of `LLVMBackend::Analyze` + `LspSymbolIndex`. Example: `cflat.exe --symbol list --symbol "dictionary.set"`.
- `--check`: Check one or more source files for errors without emitting any output. Treats *every* positional as an independent `.cb` source (no `-o`/`--out-lli`/`--bitcode`, no linking). A single backend is reused across the files - `ResetForReanalysis` clears per-file state between them while the core-library parse cache persists, so `runtime.cb` and its transitive core imports are parsed once for the whole batch. A failing file does not abort the batch; the process exit code is non-zero if any file failed. Used by `test_err.bat` to batch the `err_*.cb` negative tests into one process (amortizes both process-spawn and standard-library parsing).
- `--init`: Pre-build the compiler cache in `%USERPROFILE%\.cflat\` and exit. Caches linker paths (x64 and x86) and pre-compiles all core `.cb` libraries to LLVM bitcode for win64. Run once after installing or updating cflat; subsequent compiles load from cache instead of re-parsing core libraries (~44% faster cold-start). See [`doc/CACHING.md`](doc/CACHING.md).
- `--c-include <dir>`: Header search dir for C library bindings (repeatable)
- `--c-lib <path>`: Prebuilt C import library (.lib) to link (repeatable)
- `--c-define <NAME[=val]>`: Preprocessor define passed to all clang-cl C compiles/dumps (repeatable)
- `--c-header-cache-deep`: For C headers opted into the disk cache with the inline `cache` clause, validate every transitively included file (mtime/hash) rather than just the top header. Slower to validate; catches in-place SDK header edits. No effect on headers without a `cache` clause.

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

Examples include:
- **Visualization**: `bitmap.cb` (sixel/half-block image renderer), `stars.cb` (ASCII art starfield)
- **Games**: `minesweeper.cb`, `tetris.cb`, `missile_defender.cb`
- **CLI tools**: `shell/wc.cb`, `shell/cmd_cb.cb`
- **HTTP/REST**: `restAPI/test_http.cb`, `restAPI/test_http_server.cb`

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

test.bat uses wildard `Test/*.*` to locate tests.  Remember to remove debug test or else it would be picked up by the wildcard expansion.

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
- `parseTreeCache_`: timestamp-validated cache of parsed ANTLR trees for **implicit core-library imports only** (files under `runtimeDir/core`). Reused across compiles and LSP re-analyses since core content is stable; deliberately **not** cleared by `ResetForReanalysis` (parsing the stdlib closure dominates a small compile, so amortizing it is the big win for `--check` batches and LSP responsiveness). User imports are parsed fresh into `importedParseStates` (per-compile, cleared on reset). `ResetForReanalysis` must clear *all* transient per-call state (e.g. `lastCallIsBonded`) or a value left set by an aborted compile leaks into the next file's analysis.

`NamedVariable` has an `IsOwning` flag; `TypeAndValue` has an `IsMove` flag - both drive the ownership/lifetime system.

### Language Features

For a full language reference, see [`doc/LANGUAGE.md`](doc/LANGUAGE.md).

- **Generics**: `struct Box<T> { T value = default; }` - monomorphized at compile time
- **Interfaces**: `interface IReadable { int Read(); }` with VTable dispatch; fat pointer layout `{i8* vtable, i8* data}`
- **Namespaces**: `namespace Math { ... }` with qualified access `Math.square()`; dotted form `namespace os.windows { ... }` declares the nested chain. An `extern` inside a namespace registers under the qualified lookup name but keeps its bare linkage symbol - core's `os.windows.Sleep` and a header-imported bare `Sleep` bind to the same imported function. Core's Win32/CRT bindings all live in `os.windows`, so `import "windows.h"` registers its structs (CONSOLE_SCREEN_BUFFER_INFO, INPUT_RECORD, ...) unshadowed.
- **Type aliases**: `using M = Math` - expands at reference time
- **Module system**: `import "file.cb"` for multi-file compilation; `import { "a.cb", "b.cb" };` is a grouped shorthand for several plain imports on one line (each entry routes exactly like a bare `import "x";` - same `.cb`/`.h`/`.c` dispatch, no per-entry `lib`/`define`). A trailing `cache` on a group applies to **every** entry (`import { "a.h", "b.h" } cache;` caches both - a no-op for `.cb`/`.c` entries). `as` is meaningful only on a single-entry group (one alias cannot name many files).
- **Sized integers**: `i8, i16, i32, i64, u8, u16, u32, u64`; C aliases map to fixed widths
- **Implicit upcast, no implicit downcast**: widening conversions (e.g. `i32` -> `i64`, derived -> base/interface) happen implicitly; narrowing conversions (downcasts) require an explicit cast
- **Null-safe access**: `ptr?.field` - works on any struct pointer (local variables, function arguments, struct member fields)
- **Null-coalescing**: `a ?? b` (zero-check for integers)
- **Ternary operator**: `condition ? trueValue : falseValue` - compact conditional expression; both branches must be compatible types
- **`break` / `continue`**: exit or advance the nearest enclosing `while`, `for`, `foreach` loop
- **Operator overloading**: `operator+`, `operator==`, `operator new`, `operator delete`
- **Function overloads** with type-based resolution and default parameters
- **Named parameters**: `func(x: 1, y: 2)` - args matched by name, any order
- **Return-block functions**: `return { ... }` - body inlined at call site
- **Intrinsics**: `typeof()`, `nameof()`, `sizeof()`, `alignof()`
- **Range-based for**: `foreach (T x in collection)` - calls `count()` / `get(int)` on the collection
- **Program**: `program Name { fields...; int main(move list<string> args) { ... } };` - struct-like construct with a managed entry point; instantiate with `Name p;`, configure fields, then call `p.run(args)`. The compiler auto-generates `run()`, which spawns a dedicated thread, installs a per-thread `MallocAllocator` by default (override by setting `p._allocator` before `p.run()`), calls `main`, joins the thread, and returns the exit code. Requires `import "list.cb"` and `import "thread.cb"`.
- **Fixed-size arrays**: Use type-first syntax `T[N] name` - the size belongs to the type, not the name. C-style `T name[N]` is a compiler error for non-pointer types. Initialization forms:
  - `T[N] name;` - declared, not initialized
  - `T[N] name = default;` - zero-initialized
  - `T[N] name {}` or `T[N] name = {}` - value-initializes all N elements by calling the default constructor (struct types only)
  - `T[N] name {field=v, ...}` - seeded value-init; overrides the listed fields in each element
  - `T[N] name = {v0, v1, ...}` - **positional** element list; fewer than N zero-fills the trailing slots, more than N is an error (`EmitPositionalFixedArrayInit` in `MainListener.h`)
  - Struct fields still use C-style `T name[N]` (grammar limitation at struct scope)
  - Pointer arrays use `T*[N] name` (type-first); C-style `T* name[N]` is still accepted (exempt from the C-style error since `T*[N]` was not always available)
- **Length-inferred array-view**: `T[] name = {v0, v1, ...}` infers N from the element count. `T[]` is a thin `T*` (IsArrayView), so the brace list both allocates an inline `[N x T]` backing array and points the `T*` at element 0; N is compile-time only. Empty `{}` is an error (`EmitArrayViewInferredInit` in `MainListener.h`).
- **Container brace init**: `list<T> x = {a, b}` (-> `add()`), `array<T> x = {a, b}` (-> `init(N)`+`set()`), `dictionary<K,V> x = {k: v}` (-> `set()`). char* literals coerce to `string`; otherwise elements follow normal conversion rules (widening implicit, **no implicit down-convert** - `list<float> = {1.5}` is rejected). Empty `{}` yields an empty container (`TryEmitContainerInitializer` in `MainListener.h`). All brace-init paths above are **local-scope only**; globals still error.

### Ownership / Lifetime (`move` keyword)

CFlat uses **context-based ownership**: local variables and struct fields own their pointers; function parameters borrow by default.

The `move` keyword on a **parameter definition** transfers ownership into the callee:

```cflat
void consume(move Resource* r) { ... }  // r is freed when consume() returns
void borrow(Resource* r)        { ... }  // caller still owns r
```

- **Call site is silent** - no annotation needed; the caller's pointer is automatically nulled after a `move` call.
- `move` is a **soft keyword** - detected via text matching in `ParseDeclarationSpecifiers()`, not as an ANTLR lexer token (avoids breaking identifiers/methods named `move`).
- For primitive value types (int, float, bool, etc.) `move` is a no-op - ownership semantics only activate when `TypeAndValue.Pointer == true` **or** the value is an owning `string`. `string` owns a heap buffer (`_ptr` field) and is zeroed on move even though it is not a pointer type; use-after-move of `string` is a compile error like any pointer type.
- `list<T>::add(move T value)` and `dictionary<K,V>::add/set(K, move V value)` take ownership of pointer elements; `list::removeAt()` auto-frees pointer elements (destructor + delete); `dictionary::remove()` does not.

Implementation: `EmitOwningPtrCleanup()` in `LLVMBackend.h`; `EmitDestructorsForScope()` handles move-param cleanup at scope exit; `CreateOverloadedFunctionCall()` nulls the caller's storage after the call.

### Compile-Time Features

- **`if const`**: Condition evaluated at compile-time; dead branches eliminated from IR. Works at both function and file scope.
- **Compile-time macros**: `__FILE__`, `__FUNCTION__`, `__LINE__`, `__PLATFORM__` (64 or 32), `__WIN64__` / `__WIN32__` / `__WINDOWS__`, `__X86__` (1 on the x86/Intel target; guards architecture-specific intrinsics). Available globally.
- **`is_pointer(T)`**: Compile-time intrinsic that returns 1 if the type parameter T resolves to a pointer type in the current generic instantiation, 0 otherwise. Use with `if const` to branch on element ownership in generic code.
- **Platform support**: `-p win64` (default) or `-p win32`; guard with `if const (__PLATFORM__ == 64) { ... }`

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

The `core/` directory is implicitly added to the import search path by the compiler. Only `runtime.cb` is automatically imported; all other core libraries must be explicitly imported with `import "filename.cb"`.

| File | Exports |
|------|---------|
| `runtime.cb` | Allocator hooks (`new`, `delete`); exit/abort - **auto-imported** |
| `interfaces.cb` | `IString`, `IEnumerable<T>`, `IComparable<T>` - requires `import "interfaces.cb"` |
| `string.cb` | `string` value type, manipulation, `IString` implementation - requires `import "string.cb"` |
| `array.cb` | `array<T>` - fixed-size heap array; `init(n)`, `get(i)`, `set(i, v)`, `operator[]`; iterable with `for` |
| `span.cb` | `span<T>` - non-owning NOALIAS window (`T[] _ptr` + len); pass by value, `data()` to a local `T[]` for vectorization; `as_view()` decays to `view<T>`. See [`doc/HPC.md`](doc/HPC.md) |
| `view.cb` | `view<T>` - non-owning MAY-ALIAS window (`T* _ptr` + len); `slice(start,end)` for sub-ranges; the permissive sibling of `span<T>` |
| `list.cb` | `list<T>` - growable array; `add(move T)`, `get()`, `set(move T)`, `removeAt()` |
| `hashset.cb` | `hashset<T>` - open-addressed set; T must be integer-like |
| `dictionary.cb` | `dictionary<K,V>` - hash map; `add(K, move V)`, `set(K, move V)`, `get()`, `remove()` |
| `math.cb` | `Math` namespace: `abs`, `min`, `max`, `pow`, `sqrt`, `clamp`, `fma` (fused multiply-add), trig, rounding; constants `Math.PI` / `TAU` / `E` / `SQRT2` / `LN2` / `LN10` |
| `stack.cb` | `stack<T>` - LIFO; `push()`, `pop()`, `peek()` |
| `queue.cb` | `queue<T>` - FIFO; `enqueue()`, `dequeue()`, `peek()` |
| `pair.cb` | `pair<A,B>` - two-field generic struct |
| `filesystem.cb` | `File.Exists()`, `File.ReadAllText()`, `File.WriteAllText()`, `File.move()` |
| `thread.cb` | `thread<T>` - Win32 thread wrapper; requires `import "thread.cb"` |
| `random.cb` | Splitmix64 PRNG; requires `import "random.cb"` |
| `channel.cb` | `channel<T>` - MPMC blocking channel; requires `import "channel.cb"` |
| `spsc_queue.cb` | `spsc_queue<T>` - wait-free single-producer/single-consumer ring buffer |
| `mutex.cb` | `mutex`, `atomic<T>`, `lock` statement - thread synchronization primitives |
| `latch.cb` | `latch` - one-shot countdown synchronization |
| `semaphore.cb` | `semaphore` - counting semaphore |
| `rwlock.cb` | `rwlock` - reader-writer lock |
| `stop_token.cb` | `stop_token` / `stop_source` - cooperative cancellation |
| `threadpool.cb` | `ThreadPool` - priority work queue with `submit`/`then`/`drain`; `TaskHandle`, `TaskResult<T>` |
| `hpc/parallel.cb` | `parallel_for_n` / `parallel_reduce<T>` (raw-thread) and `parallel_for_n_pool` / `parallel_reduce_pool<T>` (ThreadPool) - across-core data parallelism over `[0,n)`; `import "hpc/parallel.cb"`. See [`doc/HPC.md`](doc/HPC.md) |
| `time.cb` | `TimePoint.now()`, `Stopwatch`, `Timer`, `sleep`, `rdtscp()` (x86 cycle counter for jitter timing), `lfence()` (x86 serializing load fence), duration utilities |
| `intrinsic.cb` | Compiler intrinsics: `popcount`, `ctz`, `clz`, `prefetch`, `likely`/`unlikely` (target-independent), `pause()` (x86 spin-loop hint, used by `spsc_queue`/`channel` spin loops) |
| `tuple.cb` | `tuple<A,B,C>` - fixed-arity generic struct |
| `json.cb` | `JsonBuilder.toJson<T>` / `fromJson<T>` - JSON serialization |
| `regex.cb` | `Regex.compile` / `isMatch` / `find` / `findAll` / `replace` / `split`, capture groups, case-insensitive - NFA (Thompson, linear-time) regex; requires `import "regex.cb"`. See [`doc/REGEX.md`](doc/REGEX.md) |
| `arena.cb` | Arena bump allocator |
| `block_allocator.cb` | Block-based pooled allocator (used by `program` thread startup) |
| `malloc_allocator.cb` | Thin wrapper around system malloc/free |
| `fit_allocator.cb` | Fit allocation strategy |
| `bucket_allocator.cb` | Segregated free-list allocator (size-class buckets) |
| `program.cb` | `program` construct runtime support (thread + allocator lifecycle) |
| `cruntime.cb` | Raw C runtime bindings: formatted I/O (`printf`/`fprintf`/`sprintf`/`snprintf`/`scanf`/`sscanf`/`fscanf`), stdout convenience writers (`puts`, `putchar`, `putn` for raw bulk bytes, `fputs`, `fputc`/`putc`), stdin readers (`getchar`, `getc`), and `string.h`/`ctype.h` (memcpy, strlen, etc.). Stdout/stdin functions are hook-aware (work inside a `program`) and VT-correct. Use `putn(buf, len)` for bulk terminal output instead of Win32 `WriteFile`. |

To add a new core library: add the `.cb` file to `core/` and add an entry in cflat.vcxproj with DeploymentContent.
Use nullptr instead of null;
Always assign "default" to fields.

### VS Code Extension

```bash
cd vscode-extension
build.bat    # compile
install.bat  # install into VS Code
```

Reload VS Code to activate syntax highlighting for `.cb` files.

## Performance Benchmarking

### Running benchmarks

Always use `performance.bat` - it compiles all benchmarks with `-O2` and runs both the CFlat and C++ reference variants:

```bash
performance.bat
```

Never compile benchmarks manually without `-O2`. Unoptimized numbers are misleading - the single-threaded loop drops from ~5B to ~300M iter/s without it, and stream throughput roughly halves.

Manual compile (if needed):

```bash
x64/Debug/cflat.exe performance/perf_yes_compare.cb -i cflat/core -o performance/perf_yes_compare.exe -O2
```

### Benchmark files

| File | Description |
|------|-------------|
| `performance/perf_yes_compare.cb` | 6 CFlat variants: channel+program, spsc+program, channel+raw Thread, stream+program, stream+raw Thread, single-threaded loop |
| `performance/perf_yes_compare.cpp` | 2 C++ reference variants: raw threads + double-buffered stream, single-threaded loop |

### Reference throughput (on a typical dev machine, `-O2`)

| Variant | CFlat | C++ |
|---------|-------|-----|
| single-threaded loop | ~5,000M iter/s | ~4,960M iter/s |
| stream + raw Thread | ~295M lines/s | ~343M lines/s |
| stream + program | ~76M lines/s | - |
| channel + raw Thread | ~12M lines/s | - |
| channel + program | ~46M lines/s | - |
| spsc + program | ~28M lines/s | - |

**Notes on stream + program**: producer uses `_out.write_bytes()` directly (bypasses `printf`/TLS entirely; set by `>>` operator). Consumer uses `_in.read_buf()` directly (bypasses `fgets`/`__stdin_hook`/TLS; set by `>>` operator). Gap vs raw-thread (~295M) is stop_token polling overhead on the producer side.

**Notes on channel variants**: the channel uses two spinlocks on separate cache lines (`_head_lock` for producers, `_tail_lock` for consumers). Under 1P:1C workloads the locks never contend with each other. Each side caches the other's cursor (`_cached_tail`/`_cached_head`) so the hot path never touches the remote cache line. No shared `_count` - full/empty are detected via head/tail comparison.

### Stream design notes

`stream` (`core/stream.cb`) is a double-buffered text pipe backed by a Win32 SRW lock + two `CONDITION_VARIABLE`s - mirroring the C++ reference:

- `_cvFull`: consumer waits here when no full buffer is ready
- `_cvEmpty`: producer waits here when no empty buffer is available for the next fill
- The lock is a Win32 SRW lock (via `mutex.sleep_cv`) - lighter than CRITICAL_SECTION, matching C++'s `std::mutex` backing

For best consumer throughput, use `read_buf(int* out_len)` instead of `read()` - it exposes the buffer length so the inner loop has a known trip count that LLVM can auto-vectorize:

```cflat
int len = 0;
char* buf = g_stream.read_buf(&len);
while (buf != nullptr) {
    int i = 0;
    while (i < len) { if (buf[i] == '\n') { count = count + (i64)1; } i++; }
    g_stream.return_buffer(buf);
    buf = g_stream.read_buf(&len);
}
```

Pin producer and consumer to separate cores with `Thread.setAffinity(u64 mask)` or `prog._thread.setAffinity(mask)` to avoid OS scheduling jitter.
