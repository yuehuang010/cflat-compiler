# Contributing to cflat

The compiler builds on **Windows**, **macOS (Apple Silicon)**, and **Linux/WSL**.
The primary build system is **CMake + vcpkg** (presets in `CMakePresets.json`); the
legacy MSBuild `.slnx`/`.vcxproj` still builds on Windows but is no longer the
default path.

## Prerequisites

**Windows:**

- **[Visual Studio 2022](https://visualstudio.microsoft.com/vs/)** with the **Desktop development with C++** workload (provides MSVC, the Windows SDK, and MSBuild).
- **[Python](https://www.python.org/downloads/)** with pip, then `pip install antlr4-tools`.
- **LLVM and ANTLR4** are built automatically via vcpkg on first build (~50 minutes the first time as vcpkg compiles LLVM from source).

**macOS (Apple Silicon):**

- `brew install cmake ninja openjdk antlr pkg-config coreutils` (add `openjdk` to PATH; `coreutils` provides `gtimeout` used by `test.sh`).
- Bootstrap vcpkg and `vcpkg install --triplet=arm64-osx` (builds LLVM + ANTLR).

**Linux / WSL (Ubuntu):**

- `apt install clang llvm-18-dev cmake ninja-build default-jre nlohmann-json3-dev libsimdjson-dev libantlr4-runtime-dev uuid-dev`, plus the ANTLR 4.10.1 jar in `/opt/antlr`. The Linux preset uses the system LLVM-18 + apt ANTLR runtime rather than vcpkg.

See [`CLAUDE.md`](../CLAUDE.md) and `CMakePresets.json` for the full per-platform details.

## Building the Compiler

**Windows** - `buildAndRun.bat` builds Debug + Release (via `cmake_build.bat`) and runs the compiler on a test file:

```bash
./buildAndRun.bat
```

**macOS / Linux** - build a preset directly:

```bash
# macOS arm64
cmake --preset macos-arm64-release && cmake --build --preset macos-arm64-release

# Linux / WSL
cmake --preset linux-x64-release && cmake --build --preset linux-x64-release
```

## Running Tests

**Windows** - `test.bat` runs the full end-to-end test suite (all tests in `Test/`):

```bash
./test.bat            # Release (default); pass Debug to override
```

**macOS / Linux** - `test.sh` is the counterpart: it runs the platform-portable
subset of `Test/*.cb` (plus the `Test/errors/*.cb` negative tests) against the
native binary, skipping Windows-only tests.

```bash
./test.sh Release     # or Debug, or -j N
```

> After any portability change, re-verify BOTH: `test.sh` on macOS/WSL and `test.bat` (Release) on Windows.

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
