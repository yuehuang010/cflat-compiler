# Copilot Instructions for MyCompiler

## Project Overview
MyCompiler is a C-dialect compiler for the CFlat language (`.cb` files), targeting LLVM IR. It's a Visual Studio 2022 C++ project using vcpkg (ANTLR4, LLVM).

## Build, Test, Run

**Build** — always use the solution file (`.vcxproj` alone puts the exe in the wrong location for `test.bat`):
```
msbuild MyCompiler.slnx -p:Configuration=Debug -p:Platform=x64
```
In Git Bash use `-p:` (dash), not `/p:` (slash) — Git Bash mangles slash-prefixed flags.

**Quick dev loop:**
```
buildAndRun.bat   # builds and runs compiler on test_generics.cb
```

**Run all tests:**
```
test.bat
```
Tests live in `Test/`. `.cb` tests are invoked with `-i Test\library`. Excluded from the suite: `test_helper`, `test_queue`, `test_stack`.

**Run a single test:**
```
x64\Debug\MyCompiler.exe Test\test_operators.cb -o out\test_operators.exe --out-lli out\test_operators.ll
out\test_operators.exe
```

**Compiler flags:** `-o` (exe), `-l/--out-lli` (IR), `-b` (bitcode), `-g` (DWARF debug), `-i` (import dir), `-p` (platform: `x64`/`x86`), `-v` (verbose), `-O1`/`-O2` (optimization).

## Architecture

### Compilation Pipeline
```
Source (.cb) → CFlatLexer/CFlatParser (ANTLR4) → Parse Tree
    → ForwardRefScanner (pre-pass)
    → MyListener (code generation)
    → LLVM Module → .ll / .bc → native .exe
```

### Two-Pass Design
`MyListener.h` contains **both** passes as separate classes:

1. **ForwardRefScanner** — pre-registers struct shells, function signatures, and generic instantiations. Enables forward references and monomorphizes generics (`Box<int>` → mangled name `Box__int`, double-underscore separator). Also detects `move` parameters and `if const` blocks.
2. **MyListener** — walks the AST and emits LLVM IR via `MyCompilerLLVM`.

**Critical:** Both passes contain their own copy of `ParseDeclarationSpecifiers()`. Any type-parsing change must be made in **both** copies.

### Key Files

| File | Role |
|------|------|
| `CFlat.g4` | ANTLR4 grammar (~1,200 lines) |
| `MyCompilerLLVM.h/.cpp` | Type system, symbol tables, IR generation |
| `MyListener.h` | Both AST passes (~4,100 lines) |
| `CompilerManager.h` | Crash handler: CRT assert hook, SIGABRT, LLVM fatal error — dumps state on crash |
| `ArgParser.h` | CLI argument parsing |
| `MyCompiler.cpp` | Entry point |
| `core/` | Standard library — auto-compiled with every program |

### Key Internal State (in `MyCompilerLLVM`)
- `stackNamedVariable` — deque of scopes for local variables
- `dataStructures` — struct registry (fields, destructor, VTables)
- `functionTable` — overload registry
- `interfaceTable` — interface method contracts
- `returnBlockTable` — inlined return-block bodies
- `NamedVariable.IsOwning` / `TypeAndValue.IsMove` — drive the ownership system

## Conventions & Patterns

### Adding Language Features

| Goal | Where |
|------|-------|
| New syntax | Edit `CFlat.g4` (rebuild regenerates ANTLR output) |
| New type / IR op | Add to `MyCompilerLLVM.h` |
| New statement / expression | Add `Parse*()`/`exit*()` handler in `MyListener.h` |
| Forward-declare a construct | Add scan logic to `ForwardRefScanner` in `MyListener.h` |
| New binary operator | `TryBinaryOperatorOverload()` in `MyListener.h` + `Operation` enum in `MyCompilerLLVM.h` |
| New soft keyword | Text-match in **both** `ParseDeclarationSpecifiers()` copies — do NOT add to the ANTLR lexer |

### Soft Keywords
`move` is not an ANTLR lexer token — it's detected by text matching in `ParseDeclarationSpecifiers()`. This avoids breaking identifiers/methods named `move`. All new keywords that could collide with identifiers must follow the same pattern.

### Ownership (`move`)
`move` on a parameter definition transfers ownership into the callee; the caller's pointer is automatically nulled after the call. For value types, `move` is a no-op. Implementation: `EmitOwningPtrCleanup()`, `EmitDestructorsForScope()`, `CreateOverloadedFunctionCall()`.

### Standard Library (`core/`)
Auto-compiled with every program. To add a new module: add the `.cb` file to `core/` and add an auto-import in `MyListener.h` (follow the `interfaces.cb` import pattern).

| File | Exports |
|------|---------|
| `runtime.cb` | `new`, `delete`, exit/abort |
| `interfaces.cb` | `IString`, `IEnumerable<T>`, `IComparable<T>` |
| `string.cb` | `string` value type (`{ i8* _ptr, i32 _len }`) |
| `list.cb` | `list<T>` — `add(move T)`, `get()`, `removeAt()` |
| `dictionary.cb` | `dictionary<K,V>` — `add(K, move V)`, `get()`, `remove()` |
| `math.cb` | `Math` namespace |
| `hashset.cb`, `stack.cb`, `queue.cb`, `pair.cb`, `filesystem.cb` | Other collections and utilities |

### Known Limitations
- `?.` null-conditional: only for pointer function arguments, not locals or fields
- Generic `V x = default` in function locals / nested generic fields may not codegen correctly; field-level `= default` works
- `removeAt()` does not auto-free owned pointer elements — caller must retrieve and free manually

## Debugging Crashes
- `CompilerManager.h` dumps compiler state on assert/abort automatically
- `-v` for verbose diagnostics; `--out-lli` to inspect generated IR
- LLVM assertion `"Ptr must have pointer type"` → `GetType()` was called without `allowPointer=true` for a pointer parameter

## VS Code Extension
```
cd vscode-extension
build.bat    # compile
install.bat  # install
```
Reload VS Code to activate `.cb` syntax highlighting.
