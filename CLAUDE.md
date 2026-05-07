# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

cflat is a C-dialect compiler targeting LLVM IR. It compiles CFlat (.cb files) — an extended C language with modern features — generics, interfaces, namespaces, operator overloading, ownership/lifetime, and null-safe access — to LLVM Intermediate Representation for native execution.

## Plan

Provide an Architecture overview before analyzing code changes.  Verify with the user first before continuing with the Phases.

## Logging Conventions

- Use `LogError` or `LogErrorContext` for all error reporting in the compiler. Do not introduce `LogWarning` or leave `std::cout`.

## Debugging Workflow

- Before editing, state the hypothesized root cause and verify against the codebase. Avoid speculative edits based on a single guess (e.g., do not assume lexer token conflicts before checking parser/grammar paths).

- After finding the root cause of the issue, consider writing a regression test.
- When encountering a LLVM assert, after identifying the root cause, then write an proper error message in the compiler to avoid that case.

## Building

Visual Studio 2022 project with vcpkg dependencies (ANTLR4, LLVM) and deploy core libraries.  Always build via the **solution file** — building the `.vcxproj` alone puts the exe in the wrong location for `test.bat`:

```bash
msbuild cflat.slnx -p:Configuration=Debug -p:Platform=x64
```

> **Bash / Git Bash note**: Use **`-p:`** (dash), not `/p:` (slash). Git Bash path-converts arguments that start with `/letter:`, stripping the leading slash, which causes MSBuild to misparse the flags. Dashes are safe.

Full msbuild path (if not on PATH):

```bash
"/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/amd64/MSBuild.exe" cflat.slnx -p:Configuration=Debug -p:Platform=x64 -v:minimal
```

**Quick dev loop** — `buildAndRun.bat` builds the solution and immediately runs the compiler on `cflat/Test/test_generics.cb`. Run directly (without `cmd /c`) so MSBuild output is captured:

```bash
./buildAndRun.bat
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

The compiler automatically locates `runtime.cb` next to the executable. Both `.cb` (CFlat) and `.c` (C-compatible) source files are accepted.

**CLI flags** (see `ArgParser.h`):
- `-o / --output`: Output native executable path (.exe)
- `-l / --out-lli`: Output LLVM IR file path (.ll)
- `-b / --bitcode`: Output LLVM bitcode file (.bc)
- `-g / --debug-info`: Emit DWARF debug information
- `-i / --import-dir`: Directory to search for imported modules
- `-p / --platform`: Target platform — `x64` (default) or `x86`
- `-v / --verbose`: Print detailed diagnostic messages during compilation

## Testing
- Always run `test.bat` after compiler changes to verify all tests pass before declaring work complete.
- `test.bat` runs all tests in parallel and should complete in under a minute. A test that hangs will be killed after a configurable timeout (default 120 seconds, set via `TIMEOUT_SECS` at the top of `test.bat`).
- Do NOT create separate compiler integration tests - test.bat already validates the compiler end-to-end.
- Do NOT revert changes to check if baseline is correct.  Assume all tests are passing and failed test are from the current changes.  Ask before reverting changes to validate baseline.
- When tests fail after a fix, investigate root causes; do not weaken/dilute test assertions to make them pass. Ask before disabling tests.

## Running Tests

```bash
test.bat
```

`test.bat` picks the compiler binary via `CFLAT_CONFIG` (defaults to `Debug`). Set it to `Release` to test the optimized build:

```bash
set CFLAT_CONFIG=Release   # cmd
$env:CFLAT_CONFIG="Release" # PowerShell
```

> **Pitfall**: If `CFLAT_CONFIG` is set in the shell that invokes `test.bat`, all spawned worker processes inherit it. After rebuilding with a different configuration, clear or update `CFLAT_CONFIG` so tests run against the intended binary.

### LSP Tests

Run the LSP test suite (smoke tests + fixture/scenario tests) with:

```bash
test_lsp.bat
```

LSP tests live in `vscode-extension/test/`. After any change to `LspServer.cpp`, `LspSymbolIndex.cpp`, or `MainListener.h` (symbol registration), run `test_lsp.bat` to verify LSP behaviour. These are kept separate from `test.bat`.

To run a single test manually:

```bash
x64/Debug/cflat.exe Test/test_operators.cb -i Test/library -o out/test_operators.exe --out-lli out/test_operators.ll
out\test_operators.exe
```

Current tests (all in `Test/`, all CFlat with `-i Test\library`): `test_allocators`, `test_basic`, `test_core`, `test_core_string`, `test_field_init`, `test_filesystem`, `test_fit_allocator`, `test_function_ptr`, `test_generics`, `test_interface`, `test_json`, `test_library_string`, `test_math`, `test_module`, `test_move`, `test_operators`, `test_program`, `test_random`, `test_reflect`, `test_sizeof`, `test_slab_allocator`, `test_sync`, `test_time`. (`test_helper.cb` is a shared helper, not run directly.)

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
// Bare-semicolon form — error must occur before the enclosing scope closes
extern int main()
{
    expect_error("Undefined variable foo.");
    int x = foo + 1;
}

// Scoped block form (statement scope) — error must occur inside the braces
expect_error("nullable '?' is not allowed on primitive type 'int'") {
    int? x = 0;
}

// Scoped block form (file scope) — for testing function/struct definitions
expect_error("missing a return statement") {
    int compute(int x) { int y = x * 2; }
}
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

Both passes share `ParseDeclarationSpecifiers()` — any change to type parsing must be applied in **both** the `ForwardRefScanner` copy and the main `MainListener` copy.

### Core Components

| File | Role |
|------|------|
| `CFlat.g4` | ANTLR4 grammar defining CFlat syntax |
| `LLVMBackend.h/.cpp` | Compiler engine: type system, symbol tables, LLVM IR generation |
| `MainListener.h` | AST visitor implementing both ForwardRefScanner and codegen passes |
| `CompilerManager.h` | Singleton crash handler — installs CRT assert hook, SIGABRT handler, and LLVM fatal error handler; dumps compiler state on any assert/crash |
| `ArgParser.h` | CLI argument parsing |
| `main.cpp` | Entry point |
| `LspServer.h/.cpp` | Language Server Protocol server (hover, completion, go-to-definition) |
| `LspSymbolIndex.h/.cpp` | LSP symbol index built during compilation |
| `JsonRpcLoop.h/.cpp` | JSON-RPC protocol loop for LSP communication |
| `LspTypes.h` | LSP type definitions |
| `core/` | Standard library — compiled alongside every program |

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

`NamedVariable` has an `IsOwning` flag; `TypeAndValue` has an `IsMove` flag — both drive the ownership/lifetime system.

### Language Features

For a full language reference, see [`doc/LANGUAGE.md`](doc/LANGUAGE.md).

- **Generics**: `struct Box<T> { T value = default; }` — monomorphized at compile time
- **Interfaces**: `interface IReadable { int Read(); }` with VTable dispatch; fat pointer layout `{i8* vtable, i8* data}`
- **Namespaces**: `namespace Math { ... }` with qualified access `Math.square()`
- **Type aliases**: `using M = Math` — expands at reference time
- **Module system**: `import "file.cb"` for multi-file compilation
- **Sized integers**: `i8, i16, i32, i64, u8, u16, u32, u64`; C aliases map to fixed widths
- **Null-safe access**: `ptr?.field` — works on any struct pointer (local variables, function arguments, struct member fields)
- **Null-coalescing**: `a ?? b` (zero-check for integers)
- **Operator overloading**: `operator+`, `operator==`, `operator new`, `operator delete`
- **Function overloads** with type-based resolution and default parameters
- **Named parameters**: `func(x: 1, y: 2)` — args matched by name, any order
- **Return-block functions**: `return { ... }` — body inlined at call site
- **Intrinsics**: `typeof()`, `nameof()`, `sizeof()`, `alignof()`
- **Range-based for**: `foreach (T x in collection)` — calls `count()` / `get(int)` on the collection
- **Program**: `program Name { fields...; int main(move list<string> args) { ... } };` — struct-like construct with a managed entry point; instantiate with `Name p;`, configure fields, then call `p.run(args)`. The compiler auto-generates `run()`, which spawns a dedicated thread, installs a per-thread `MallocAllocator` by default (override by setting `p._allocator` before `p.run()`), calls `main`, joins the thread, and returns the exit code. Requires `import "list.cb"` and `import "thread.cb"`.

### Ownership / Lifetime (`move` keyword)

CFlat uses **context-based ownership**: local variables and struct fields own their pointers; function parameters borrow by default.

The `move` keyword on a **parameter definition** transfers ownership into the callee:

```cflat
void consume(move Resource* r) { ... }  // r is freed when consume() returns
void borrow(Resource* r)        { ... }  // caller still owns r
```

- **Call site is silent** — no annotation needed; the caller's pointer is automatically nulled after a `move` call.
- `move` is a **soft keyword** — detected via text matching in `ParseDeclarationSpecifiers()`, not as an ANTLR lexer token (avoids breaking identifiers/methods named `move`).
- For value types (int, etc.) `move` is a no-op — ownership semantics only activate when `TypeAndValue.Pointer == true`.
- `list<T>::add(move T value)` and `dictionary<K,V>::add/set(K, move V value)` take ownership of pointer elements; `list::removeAt()` auto-frees pointer elements (destructor + delete); `dictionary::remove()` does not.

Implementation: `EmitOwningPtrCleanup()` in `LLVMBackend.h`; `EmitDestructorsForScope()` handles move-param cleanup at scope exit; `CreateOverloadedFunctionCall()` nulls the caller's storage after the call.

### Compile-Time Features

- **`if const`**: Condition evaluated at compile-time; dead branches eliminated from IR. Works at both function and file scope.
- **Compile-time macros**: `__FILE__`, `__FUNCTION__`, `__LINE__`, `__PLATFORM__` (64 or 32). Available globally.
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
| New soft keyword (like `move`) | Text-match in both `ParseDeclarationSpecifiers()` copies in `MainListener.h` — do NOT add to the ANTLR lexer |
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
| `runtime.cb` | Allocator hooks (`new`, `delete`); exit/abort — **auto-imported** |
| `interfaces.cb` | `IString`, `IEnumerable<T>`, `IComparable<T>` — requires `import "interfaces.cb"` |
| `string.cb` | `string` value type, manipulation, `IString` implementation — requires `import "string.cb"` |
| `array.cb` | `array<T>` — fixed-size heap array; `init(n)`, `get(i)`, `set(i, v)`, `operator[]`; iterable with `for` |
| `list.cb` | `list<T>` — growable array; `add(move T)`, `get()`, `set(move T)`, `removeAt()` |
| `hashset.cb` | `hashset<T>` — open-addressed set; T must be integer-like |
| `dictionary.cb` | `dictionary<K,V>` — hash map; `add(K, move V)`, `set(K, move V)`, `get()`, `remove()` |
| `math.cb` | `Math` namespace: `abs`, `min`, `max`, `pow`, `sqrt`, `clamp`, trig, rounding |
| `stack.cb` | `stack<T>` — LIFO; `push()`, `pop()`, `peek()` |
| `queue.cb` | `queue<T>` — FIFO; `enqueue()`, `dequeue()`, `peek()` |
| `pair.cb` | `pair<A,B>` — two-field generic struct |
| `filesystem.cb` | `File.Exists()`, `File.ReadAllText()`, `File.WriteAllText()`, `File.move()` |
| `thread.cb` | `thread<T>` — Win32 thread wrapper; requires `import "thread.cb"` |
| `random.cb` | Splitmix64 PRNG; requires `import "random.cb"` |
| `channel.cb` | `channel<T>` — MPMC blocking channel; requires `import "channel.cb"` |
| `spsc_queue.cb` | `spsc_queue<T>` — wait-free single-producer/single-consumer ring buffer |
| `mutex.cb` | `mutex`, `atomic<T>`, `lock` statement — thread synchronization primitives |
| `latch.cb` | `latch` — one-shot countdown synchronization |
| `semaphore.cb` | `semaphore` — counting semaphore |
| `rwlock.cb` | `rwlock` — reader-writer lock |
| `stop_token.cb` | `stop_token` / `stop_source` — cooperative cancellation |
| `time.cb` | `Time.now()`, sleep, duration utilities |
| `tuple.cb` | `tuple<A,B,C>` — fixed-arity generic struct |
| `json.cb` | `JsonBuilder.toJson<T>` / `fromJson<T>` — JSON serialization |
| `arena.cb` | Arena bump allocator |
| `block_allocator.cb` | Block-based pooled allocator (used by `program` thread startup) |
| `malloc_allocator.cb` | Thin wrapper around system malloc/free |
| `fit_allocator.cb` | Fit allocation strategy |
| `slab_allocator.cb` | Slab allocator for fixed-size objects |
| `program.cb` | `program` construct runtime support (thread + allocator lifecycle) |
| `cruntime.cb` | Raw C runtime bindings (printf, memcpy, etc.) |

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

Always use `performance.bat` — it compiles all benchmarks with `-O2` and runs both the CFlat and C++ reference variants:

```bash
performance.bat
```

Never compile benchmarks manually without `-O2`. Unoptimized numbers are misleading — the single-threaded loop drops from ~5B to ~300M iter/s without it, and stream throughput roughly halves.

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
| stream + program | ~76M lines/s | — |
| channel + raw Thread | ~12M lines/s | — |
| channel + program | ~46M lines/s | — |
| spsc + program | ~28M lines/s | — |

**Notes on stream + program**: producer uses `_out.write_bytes()` directly (bypasses `printf`/TLS entirely; set by `>>` operator). Consumer uses `_in.read_buf()` directly (bypasses `fgets`/`__stdin_hook`/TLS; set by `>>` operator). Gap vs raw-thread (~295M) is stop_token polling overhead on the producer side.

**Notes on channel variants**: the channel uses two spinlocks on separate cache lines (`_head_lock` for producers, `_tail_lock` for consumers). Under 1P:1C workloads the locks never contend with each other. Each side caches the other's cursor (`_cached_tail`/`_cached_head`) so the hot path never touches the remote cache line. No shared `_count` — full/empty are detected via head/tail comparison.

### Stream design notes

`stream` (`core/stream.cb`) is a double-buffered text pipe backed by a Win32 SRW lock + two `CONDITION_VARIABLE`s — mirroring the C++ reference:

- `_cvFull`: consumer waits here when no full buffer is ready
- `_cvEmpty`: producer waits here when no empty buffer is available for the next fill
- The lock is a Win32 SRW lock (via `mutex.sleep_cv`) — lighter than CRITICAL_SECTION, matching C++'s `std::mutex` backing

For best consumer throughput, use `read_buf(int* out_len)` instead of `read()` — it exposes the buffer length so the inner loop has a known trip count that LLVM can auto-vectorize:

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
