# C ♭(Flat)

A C-dialect language with modern features, compiled to LLVM IR.

CFlat (`.cb`) extends C with generics, interfaces, namespaces, operator overloading, and null-safety - while keeping C's familiar syntax and control.

## Quick Start

cflat is self-contained on Windows 10 and later - it links native executables without the Windows SDK or Visual Studio. The hello app below builds and runs on a clean machine. (The Windows SDK is only needed for advanced C interop against prebuilt system libraries; cflat falls back to it automatically when present.)

```bash
# Compile a source file to native executable
cflat.exe hello.cb -o hello.exe
```

```c
import "list.cb";

extern int main() {
    list<int> nums;
    nums.add(42);
    printf("First: %d\n", nums.get(0));
    return 0;
}
```

### VS Code Setup

1. Install the extension: in VS Code, open the Command Palette (`Ctrl+Shift+P`) -> **Extensions: Install from VSIX...** and pick `vscode-extension/cflat-language-*.vsix`.
2. Point the extension at your compiler: set [`cflat.executablePath`](vscode://settings/cflat.executablePath) to the full path of `cflat.exe`.

## Documentation

Start with **[Language & Core Library Features](doc/LANGUAGE.md)** - the complete language reference (types, functions, generics, interfaces, namespaces, memory management, null-safety, introspection, and compiler CLI options). It links out to the focused references: C interop, threading & memory, and HPC & SIMD.

- **[Contributing to cflat](doc/CONTRIBUTING.md)** - Building the compiler, development workflow, and running tests.
