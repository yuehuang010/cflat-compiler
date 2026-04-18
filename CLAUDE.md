# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

MyCompiler is a C-dialect compiler targeting LLVM IR. It compiles CFlat (.cb files) — an extended C language with modern features — generics, interfaces, namespaces, operator overloading, and null-safe access — to LLVM Intermediate Representation for JIT execution.

## Building

Visual Studio 2022 project with vcpkg dependencies (ANTLR4, LLVM):

```bash
msbuild MyCompiler/MyCompiler.vcxproj /p:Configuration=Debug /p:Platform=x64
```

**Quick dev loop** — `buildAndRun.bat` builds the solution and immediately runs the compiler on `MyCompiler/Test/test_filesystem.cb`, producing `myapp.exe` and `out.ll`:

```bash
buildAndRun.bat
```

## Running

```bash
# Compile to native executable
x64/Debug/MyCompiler.exe input.cb -o out.exe

# Also dump LLVM IR alongside the exe
x64/Debug/MyCompiler.exe input.cb -o out.exe --out-lli out.ll

# With options
MyCompiler.exe input.cb -o out.exe -b out.bc -g -i import-dir/

# Execute IR directly (skip -o, dump IR with --out-lli, run via lli)
x64/Debug/MyCompiler.exe input.cb --out-lli out.ll
lli.exe out.ll
```

The compiler automatically locates `runtime.cb` next to the executable (built from `MyCompiler/core/runtime.cb`). Both `.cb` (CFlat) and `.c` (C-compatible) source files are accepted.

**CLI flags** (see `ArgParser.h`):
- `-o / --output`: Output native executable path (.exe)
- `-l / --out-lli`: Output LLVM IR file path (.ll)
- `-b / --bitcode`: Output LLVM bitcode file (.bc)
- `-g / --debug-info`: Emit DWARF debug information
- `-i / --import-dir`: Directory to search for imported modules
- `-p / --platform`: Target platform — `x64` (default) or `x86`
- `-v / --verbose`: Print detailed diagnostic messages during compilation

## Running Tests

```bash
test.bat
```

Runs the compiler against test files in `MyCompiler/Test/` — compiles each to a native `.exe` and checks the exit code. Current tests: `testfile` (C); `testfile2`, `test_generics` (CFlat); `testfile_module`, `test_library_string` (CFlat, with `-i` lib); `test_operators`, `test_is_as`, `test_core`, `test_core_string`, `test_filesystem`, `test_static` (CFlat).

To run a single test manually:

```bash
x64/Debug/MyCompiler.exe MyCompiler/Test/test_operators.cb -o out/test_operators.exe --out-lli out/test_operators.ll
out\test_operators.exe
```

## Architecture

### Compilation Pipeline

```
Source (.cb) → CFlatLexer/CFlatParser (ANTLR4) → Parse Tree
    → ForwardRefScanner (pre-pass)
    → MyListener (code generation)
    → LLVM Module → .ll / .bc
```

### Two-Pass Compilation

1. **ForwardRefScanner** (`MyListener.h`): Pre-registers struct shells, function signatures, and generic instantiations. This enables forward references and monomorphizes generics (e.g., `Box<int>` → symbol `Box__int`) before code generation begins.
2. **MyListener** (`MyListener.h`): Walks the AST and emits LLVM IR using `MyCompilerLLVM` as the backend.

### Core Components

| File | Role |
|------|------|
| `CFlat.g4` | ANTLR4 grammar defining CFlat syntax (~1,200 lines) |
| `MyCompilerLLVM.h/.cpp` | Compiler engine: type system, symbol tables, LLVM IR generation |
| `MyListener.h` | AST visitor implementing both passes (~4,100 lines) |
| `CompilerManager.h` | Singleton crash handler — installs CRT assert hook, SIGABRT handler, and LLVM fatal error handler; dumps compiler state on any assert/crash |
| `ArgParser.h` | CLI argument parsing |
| `MyCompiler.cpp` | Entry point |
| `core/` | Standard library source: `runtime.cb`, `interfaces.cb`, `string.cb`, `list.cb`, `hashset.cb`, `dictionary.cb` — compiled alongside every program; `interfaces.cb` is auto-imported between runtime and string |

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

### Language Features Supported

- **Generics**: `struct Box<T> { T value = default; }` — monomorphized to `Box__int` (double-underscore mangling)
- **Interfaces**: `interface IReadable { int Read(); }` with VTable dispatch; fat pointer layout `{i8* vtable, i8* data}`
- **Namespaces**: `namespace Math { ... }` with qualified access `Math.square()`
- **Type aliases**: `using M = Math` — expands at reference time (stored in `namespaceAliasTable`)
- **Module system**: `import "file.cb"` for multi-file compilation
- **Sized integers**: `i8, i16, i32, i64, u8, u16, u32, u64`; C aliases: `char=i8`, `short=i16`, `int=i32`, `long=i64`
- **Null-safe access**: `ptr?.field` (only via function arg pointers)
- **Null-coalescing**: `a ?? b` (zero-check for integers)
- **Default initialization**: `= default` in generic structs
- **Operator overloading**: `operator+`, `operator==`, `operator new`, `operator delete`
- **Function overloads** with type-based resolution and default parameters
- **Named parameters**: `func(x: 1, y: 2)` — args matched by name, any order
- **Return-block functions**: `return { ... }` — body inlined at call site (stored in `returnBlockTable`)
- **Intrinsics**: `typeof()`, `nameof()`, `sizeof()`, `alignof()`
- **Range-based for**: `foreach (T x in collection) { ... }` — calls `count()` / `get(int)` on the collection; works with structs and `IEnumerable<T>` interface values

### Compile-Time Features

- **`if const`**: Condition evaluated at compile-time; dead branches eliminated from IR. Syntax: `if const (expr) { ... }` 
- **Compile-time macros**: `__FILE__`, `__FUNCTION__`, `__LINE__`, `__PLATFORM__` (64 or 32 based on `-p` flag). Pre-populated before compilation; available in all contexts including global scope.
- **Platform support**: `-p win64` (default, maps to 64) or `-p win32` (maps to 32). Enables platform-specific code via `if const (__PLATFORM__ == 64) { ... }`

### Known Limitations

- **Null-conditional operator (`?.`)**: Only works with pointers passed as function arguments; not available for local variables or struct members
- **Generic struct defaults**: Field-level `= default` works; nested generics and `V x = default` in function locals may not codegen correctly
- **Sized integers**: C-style aliases (`char`, `short`, `int`, `long`) map to fixed widths (`i8`, `i16`, `i32`, `i64`) — mixing styles can be confusing but is valid

### VS Code Extension

`vscode-extension/` contains a language server extension for CFlat syntax highlighting and tooling:

```bash
cd vscode-extension
build.bat        # compiles the extension
install.bat      # installs into VS Code
```

Reload VS Code to activate. The extension provides syntax highlighting for `.cb` files and integrates with the compiler for diagnostics.

### Adding New Language Features

| Goal | Where to change |
|------|----------------|
| New syntax | Edit `CFlat.g4`; rebuild triggers ANTLR regeneration |
| New type or IR operation | Add methods to `MyCompilerLLVM.h` |
| New statement or expression | Add `Parse*()` handler in `MyListener.h` |
| Forward-declare a new construct | Add scan logic to `ForwardRefScanner` in `MyListener.h` |
| New binary operator | `TryBinaryOperatorOverload()` in `MyListener.h` + `Operation` enum in `MyCompilerLLVM.h` |

### Common Development Workflows

**Adding a new statement type** (e.g., a `do-while` loop):
1. Add grammar rule to `CFlat.g4`
2. Rebuild to regenerate `CFlatParser.h` / `CFlatLexer.h`
3. Add handler in `MyListener.h` (e.g., `exitDoWhile()`)
4. Emit LLVM IR via `builder` in the handler
5. Write a test in `MyCompiler/Test/` and run `test.bat`

**Adding a new built-in function** (e.g., a math intrinsic):
1. Decide: intrinsic (compile-time, in `MyCompilerLLVM.h`) or library function (in `core/runtime.cb`)
2. Add to symbol table in `MyCompilerLLVM.h` (if compiler-special) or define in `core/runtime.cb`
3. In `MyListener.h`, add code generation (or forward to LLVM intrinsic)
4. Test with a simple `.cb` file

**Debugging compiler crashes**:
- `CompilerManager.h` installs crash handlers that dump compiler state on assert/abort
- Rerun with `-v / --verbose` to see detailed diagnostics
- Add `printf` statements in `MyListener.h` or `MyCompilerLLVM.h` to trace code generation
- Check `.ll` output (via `--out-lli`) to inspect LLVM IR before execution

### Standard Library

Core library files in `core/` are automatically compiled alongside every program:

| File | Exports |
|------|---------|
| `runtime.cb` | Allocator hooks (`new`, `delete`); exit/abort |
| `interfaces.cb` | Core interfaces: `IString`, `IEnumerable<T>`, `IComparable<T>` |
| `string.cb` | `string` value type, manipulation, and `IString` implementation |
| `list.cb` | `List<T>` — dynamic array with `add()`, `get()`, `remove()` |
| `hashset.cb` | `HashSet<T>` — hash-based set with `add()`, `contains()`, `remove()` |
| `dictionary.cb` | `Dictionary<K,V>` — key-value map with `set()`, `get()`, `remove()` |
| `filesystem.cb` | File I/O: `File.Exists()`, `File.ReadAllText()`, `File.WriteAllText()` |

Extend the standard library by adding new `.cb` files to `core/` and modifying `MyListener.h` to auto-import them if needed.
