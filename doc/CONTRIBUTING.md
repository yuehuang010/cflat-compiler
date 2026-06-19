# Contributing to cflat

## Prerequisites

- **[Visual Studio 2022](https://visualstudio.microsoft.com/vs/)** with the **Desktop development with C++** workload (provides MSVC, the Windows SDK, and MSBuild).
- **[Python](https://www.python.org/downloads/)** with pip, then install the ANTLR tool:

  ```bash
  pip install antlr4-tools
  ```

- **LLVM and ANTLR4** are built automatically via vcpkg on first build. This can take a while (~50 minutes) the first time as vcpkg compiles LLVM from source.

## Building the Compiler

**`buildAndRun.bat`** builds the solution and immediately runs the compiler on a test file:

```bash
./buildAndRun.bat
```

## Running Tests

`test.bat` runs the full end-to-end test suite (all tests in `Test/`):

```bash
./test.bat
```

## Examples and Benchmarks

`example.bat` builds every runnable example program in the `example/` tree to verify they still compile:

```bash
./example.bat
```

`performance.bat` runs the performance benchmarks to check compiler/runtime throughput:

```bash
./performance.bat
```

## VS Code Extension

The `vscode-extension/` directory holds the cflat language extension (syntax
highlighting and the LSP-backed language server). It requires [Node.js](https://nodejs.org/)
and the `code` command on your PATH.

`build.bat` compiles the extension and language server:

```bash
cd vscode-extension
./build.bat
```

`launch.bat` opens a new VS Code "Extension Development Host" window with the
extension loaded (no install needed) - the quickest way to test changes:

```bash
./launch.bat
```

`install.bat` installs the extension into your VS Code so it loads in every
session:

```bash
./install.bat
```
