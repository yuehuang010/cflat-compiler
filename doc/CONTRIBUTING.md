# Contributing to MyCompiler

## Building the Compiler

Requires Visual Studio 2022 and vcpkg (ANTLR4, LLVM):

```bash
msbuild MyCompiler/MyCompiler.vcxproj /p:Configuration=Debug /p:Platform=x64
```

> **Bash / Git Bash note**: Use **`-p`** (dash), not `/p` (slash). Git Bash path-converts arguments that start with `/letter:`, stripping the leading slash, which causes MSBuild to misparse the flags. Dashes are safe.

Full msbuild path (if not on PATH):

```bash
MSBuild.exe MyCompiler.slnx -p:Configuration=Debug -p:Platform=x64
```

## Quick Development Loop

**`buildAndRun.bat`** builds the solution and immediately runs the compiler on a test file:

```bash
./buildAndRun.bat
```

## Running Tests

```bash
./test.bat
```
