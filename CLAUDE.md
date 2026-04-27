# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

MyCompiler is a C-dialect compiler targeting LLVM IR. It compiles CFlat (.cb files) — an extended C language with modern features — generics, interfaces, namespaces, operator overloading, ownership/lifetime, and null-safe access — to LLVM Intermediate Representation for native execution.

## Plan

Provide an Architecture overview before analyzing code changes.  Verify with the user first before continuing with the Phases.

## Logging Conventions

- Use `LogError` or `LogErrorContext` for all error reporting in the compiler. Do not introduce `LogWarning` or leave `std::cout`.

## Debugging Workflow

- Before editing, state the hypothesized root cause and verify against the codebase. Avoid speculative edits based on a single guess (e.g., do not assume lexer token conflicts before checking parser/grammar paths).

## Building

Visual Studio 2022 project with vcpkg dependencies (ANTLR4, LLVM). Always build via the **solution file** — building the `.vcxproj` alone puts the exe in the wrong location for `test.bat`:

```bash
msbuild MyCompiler.slnx -p:Configuration=Debug -p:Platform=x64
```

> **Bash / Git Bash note**: Use **`-p:`** (dash), not `/p:` (slash). Git Bash path-converts arguments that start with `/letter:`, stripping the leading slash, which causes MSBuild to misparse the flags. Dashes are safe.

Full msbuild path (if not on PATH):

```bash
"/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/amd64/MSBuild.exe" MyCompiler.slnx -p:Configuration=Debug -p:Platform=x64 -v:minimal
```

**Quick dev loop** — `buildAndRun.bat` builds the solution and immediately runs the compiler on `MyCompiler/Test/test_generics.cb`. Run directly (without `cmd /c`) so MSBuild output is captured:

```bash
./buildAndRun.bat
```

## Running

```bash
# Compile to native executable
x64/Debug/MyCompiler.exe input.cb -o out.exe

# Also dump LLVM IR
x64/Debug/MyCompiler.exe input.cb -o out.exe --out-lli out.ll

# Execute IR directly via lli
x64/Debug/MyCompiler.exe input.cb --out-lli out.ll && lli.exe out.ll
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
- Do NOT create separate compiler integration tests - test.bat already validates the compiler end-to-end.
- Do NOT revert changes to check if baseline is correct.  Assume all tests are passing and failed test are from the current changes.  Ask before reverting changes to validate baseline.
- When tests fail after a fix, investigate root causes; do not weaken/dilute test assertions to make them pass. Ask before disabling tests.

## Running Tests

```bash
test.bat
```

To run a single test manually:

```bash
x64/Debug/MyCompiler.exe MyCompiler/Test/test_operators.cb -o out/test_operators.exe --out-lli out/test_operators.ll
out\test_operators.exe
```

Current tests (all in `MyCompiler/Test/`): `testfile` (C); `testfile2`, `test_generics`, `test_operators`, `test_is_as`, `test_core`, `test_core_string`, `test_filesystem`, `test_static`, `test_nullable`, `test_move` (CFlat); `testfile_module`, `test_library_string`, `test_overload` (CFlat with `-i lib`).

### Error tests

Negative tests live in `Test/errors/` and use the `expect_error` compiler built-in. `test.bat` compiles each `err_*.cb` and expects exit code 0. To run one individually:

```bash
x64/Debug/MyCompiler.exe Test/errors/err_missing_return.cb -i Test/library
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
Source (.cb) → CFlatLexer/CFlatParser (ANTLR4) → Parse Tree
    → ForwardRefScanner (pre-pass)
    → MyListener (code generation)
    → LLVM Module → .ll / .bc → native .exe
```

### Two-Pass Compilation

1. **ForwardRefScanner** (`MyListener.h`): Pre-registers struct shells, function signatures, and generic instantiations before codegen. Enables forward references and monomorphizes generics (`Box<int>` → symbol `Box__int`, double-underscore mangling). Also detects `move` parameters and `if const` blocks.
2. **MyListener** (`MyListener.h`): Walks the AST and emits LLVM IR using `MyCompilerLLVM` as the backend.

Both passes share `ParseDeclarationSpecifiers()` — any change to type parsing must be applied in **both** the `ForwardRefScanner` copy and the main `MyListener` copy.

### Core Components

| File | Role |
|------|------|
| `CFlat.g4` | ANTLR4 grammar defining CFlat syntax (~1,200 lines) |
| `MyCompilerLLVM.h/.cpp` | Compiler engine: type system, symbol tables, LLVM IR generation |
| `MyListener.h` | AST visitor implementing both passes (~4,100 lines) |
| `CompilerManager.h` | Singleton crash handler — installs CRT assert hook, SIGABRT handler, and LLVM fatal error handler; dumps compiler state on any assert/crash |
| `ArgParser.h` | CLI argument parsing |
| `MyCompiler.cpp` | Entry point |
| `core/` | Standard library — compiled alongside every program |

### Key Internal State (in `MyCompilerLLVM`)

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

- **Generics**: `struct Box<T> { T value = default; }` — monomorphized at compile time
- **Interfaces**: `interface IReadable { int Read(); }` with VTable dispatch; fat pointer layout `{i8* vtable, i8* data}`
- **Namespaces**: `namespace Math { ... }` with qualified access `Math.square()`
- **Type aliases**: `using M = Math` — expands at reference time
- **Module system**: `import "file.cb"` for multi-file compilation
- **Sized integers**: `i8, i16, i32, i64, u8, u16, u32, u64`; C aliases map to fixed widths
- **Null-safe access**: `ptr?.field` (only via function arg pointers, not locals)
- **Null-coalescing**: `a ?? b` (zero-check for integers)
- **Operator overloading**: `operator+`, `operator==`, `operator new`, `operator delete`
- **Function overloads** with type-based resolution and default parameters
- **Named parameters**: `func(x: 1, y: 2)` — args matched by name, any order
- **Return-block functions**: `return { ... }` — body inlined at call site
- **Intrinsics**: `typeof()`, `nameof()`, `sizeof()`, `alignof()`
- **Range-based for**: `foreach (T x in collection)` — calls `count()` / `get(int)` on the collection

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
- `list<T>::add(move T value)` and `dictionary<K,V>::add/set(K, move V value)` take ownership of pointer elements; element destructors are not auto-called on `removeAt()` — caller must retrieve and free manually.

Implementation: `EmitOwningPtrCleanup()` in `MyCompilerLLVM.h`; `EmitDestructorsForScope()` handles move-param cleanup at scope exit; `CreateOverloadedFunctionCall()` nulls the caller's storage after the call.

### Compile-Time Features

- **`if const`**: Condition evaluated at compile-time; dead branches eliminated from IR. Works at both function and file scope.
- **Compile-time macros**: `__FILE__`, `__FUNCTION__`, `__LINE__`, `__PLATFORM__` (64 or 32). Available globally.
- **Platform support**: `-p win64` (default) or `-p win32`; guard with `if const (__PLATFORM__ == 64) { ... }`

### Known Limitations

- **Null-conditional (`?.`)**: Only for pointers passed as function arguments — not local variables or struct members
- **Generic struct defaults**: `V x = default` in function locals and nested generic fields may not codegen correctly; field-level `= default` works
- **`move` on `removeAt`**: Owned pointer elements are not auto-freed on removal from a collection

### Adding New Language Features

| Goal | Where to change |
|------|----------------|
| New syntax | Edit `CFlat.g4`; rebuild triggers ANTLR regeneration |
| New type or IR operation | Add methods to `MyCompilerLLVM.h` |
| New statement or expression | Add `Parse*()` / `exit*()` handler in `MyListener.h` |
| Forward-declare a new construct | Add scan logic to `ForwardRefScanner` in `MyListener.h` |
| New binary operator | `TryBinaryOperatorOverload()` in `MyListener.h` + `Operation` enum in `MyCompilerLLVM.h` |
| New soft keyword (like `move`) | Text-match in both `ParseDeclarationSpecifiers()` copies in `MyListener.h` — do NOT add to the ANTLR lexer |
| New grammar keyword statement | Add rule to `CFlat.g4`; add `ctx->newRule()` retrieval in `ParseStatement`; add no-op overrides in `ForwardRefScanner` if the rule can appear at file scope |

### Debugging Compiler Crashes

- `CompilerManager.h` installs crash handlers that dump compiler state on assert/abort
- Rerun with `-v` to see detailed diagnostics
- Check `.ll` output (`--out-lli`) to inspect LLVM IR
- LLVM assertion `"Ptr must have pointer type"` usually means `GetType()` was called without `allowPointer=true` for a pointer parameter

### Standard Library

Core library files in `core/` are automatically compiled alongside every program:

| File | Exports |
|------|---------|
| `runtime.cb` | Allocator hooks (`new`, `delete`); exit/abort |
| `interfaces.cb` | `IString`, `IEnumerable<T>`, `IComparable<T>` — auto-imported between runtime and string |
| `string.cb` | `string` value type, manipulation, `IString` implementation |
| `list.cb` | `list<T>` — growable array; `add(move T)`, `get()`, `set(move T)`, `removeAt()` |
| `hashset.cb` | `hashset<T>` — open-addressed set; T must be integer-like |
| `dictionary.cb` | `dictionary<K,V>` — hash map; `add(K, move V)`, `set(K, move V)`, `get()`, `remove()` |
| `math.cb` | `Math` namespace: `abs`, `min`, `max`, `pow`, `sqrt`, `clamp`, trig, rounding |
| `stack.cb` | `stack<T>` — LIFO; `push()`, `pop()`, `peek()` |
| `queue.cb` | `queue<T>` — FIFO; `enqueue()`, `dequeue()`, `peek()` |
| `pair.cb` | `pair<A,B>` — two-field generic struct |
| `filesystem.cb` | `File.Exists()`, `File.ReadAllText()`, `File.WriteAllText()`, `File.move()` |

To add a new core library: add the `.cb` file to `core/` and add an auto-import in `MyListener.h` (search for where `interfaces.cb` is imported for the pattern).

### VS Code Extension

```bash
cd vscode-extension
build.bat    # compile
install.bat  # install into VS Code
```

Reload VS Code to activate syntax highlighting for `.cb` files.
