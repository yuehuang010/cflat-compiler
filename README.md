# MyCompiler

MyCompiler is a C compiler with object-oriented extensions that compiles source code to [LLVM IR](https://llvm.org/docs/LangRef.html). It is built in C++ using [ANTLR4](https://www.antlr.org/) for parsing and the LLVM library for code generation.

## Features

- **C11 base** with custom extensions:
  - Extended integer types: `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`
  - Object-oriented constructs: `class`, `struct` with methods, `interface`, constructors and destructors
  - `namespace` and `using` declarations
  - `import "file.c"` for multi-file compilation
  - Named function parameters (e.g., `func(x: 1, y: 2)`) and default parameter values
  - `typeof` and `nameof` operators
- **LLVM IR output** — compatible with the full LLVM toolchain (`lli`, `llc`, `clang`, etc.)
- **Optional DWARF debug info** via `--debug-info`
- **VS Code extension** (`vscode-extension/`) providing syntax highlighting and live diagnostics for `.myc` files

## Project Layout

```
MyCompiler/
  C.g4                  ANTLR4 grammar for the C dialect
  MyCompiler.cpp        Entry point – parses CLI args and drives compilation
  MyCompilerLLVM.h/cpp  Core LLVM IR emitter and type system
  MyListener.h          ANTLR4 parse-tree listener that walks the AST
  ArgParser.h           Header-only command-line argument parser
  Test/                 Test suite (.c files exercising language features)
vscode-extension/       VS Code language-server extension
MyCompiler.slnx         Visual Studio solution
vcpkg.json              vcpkg dependency manifest (antlr4, llvm)
run.bat                 Run a compiled .ll file with lli.exe
test.bat                Build and run the full test suite
```

## Building

**Requirements:** Visual Studio 2022, [vcpkg](https://vcpkg.io/), LLVM toolchain on `PATH`.

1. Install dependencies via vcpkg:
   ```bat
   vcpkg install
   ```
2. Open `MyCompiler.slnx` in Visual Studio and build the solution (x64 Release).

## Usage

```
MyCompiler.exe [options] <source.c>

Options:
  -o, --output <path>      Output LLVM IR file (default: .\out.ll)
  -b, --bitcode <path>     Output LLVM bitcode file (.bc)
  -g, --debug-info         Emit DWARF debug information
  -i, --import-dir <path>  Additional directory to search for imports
  -h, --help               Show help
```

Run the resulting IR directly with LLVM's interpreter:

```bat
run.bat out.ll
```

## Testing

```bat
test.bat
```

This compiles and executes every test file under `MyCompiler/Test/` using `lli.exe` and reports pass/fail for each suite.