# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

MyCompiler is a C-dialect compiler targeting LLVM IR. It compiles CFlat (.cb files) — an extended C language with modern features — generics, interfaces, namespaces, operator overloading, and null-safe access — to LLVM Intermediate Representation for JIT execution.

## Building

Visual Studio 2022 project with vcpkg dependencies (ANTLR4, LLVM):

```bash
msbuild MyCompiler/MyCompiler.vcxproj /p:Configuration=Debug /p:Platform=x64
```

## Running

```bash
# Compile a source file
x64/Debug/MyCompiler.exe input.cb -o out.ll

# With options
MyCompiler.exe input.cb -o output.ll -b output.bc -g -i import-dir/

# Execute the generated IR
lli.exe out.ll
```

**CLI flags** (see `ArgParser.h`):
- `-o / --output`: Output IR file (default: `.\out.ll`)
- `-b / --bitcode`: Output LLVM bitcode file (.bc)
- `-g / --debug-info`: Emit DWARF debug information
- `-i / --import-dir`: Directory to search for imported modules

## Running Tests

```bash
test.bat
```

Runs the compiler against test files in `MyCompiler/Test/` and validates output using LLVM's `lli.exe` interpreter.

To run a single test manually:

```bash
x64/Debug/MyCompiler.exe MyCompiler/Test/test_operators.cb -o out.ll
lli.exe out.ll
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
| `ArgParser.h` | CLI argument parsing |
| `MyCompiler.cpp` | Entry point |

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

### Adding New Language Features

| Goal | Where to change |
|------|----------------|
| New syntax | Edit `CFlat.g4`; rebuild triggers ANTLR regeneration |
| New type or IR operation | Add methods to `MyCompilerLLVM.h` |
| New statement or expression | Add `Parse*()` handler in `MyListener.h` |
| Forward-declare a new construct | Add scan logic to `ForwardRefScanner` in `MyListener.h` |
| New binary operator | `TryBinaryOperatorOverload()` in `MyListener.h` + `Operation` enum in `MyCompilerLLVM.h` |
