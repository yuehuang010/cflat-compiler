# C ♭(Flat)

A C-dialect language with modern features, compiled to LLVM IR.

CFlat (`.cb`) extends C with generics, interfaces, namespaces, operator overloading, and null-safety - while keeping C's familiar syntax and control.

---

## Quick Start

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

---

## Documentation

Start with **[Language & Core Library Features](doc/LANGUAGE.md)** - the complete language reference (types, functions, generics, interfaces, namespaces, memory management, null-safety, introspection, and compiler CLI options). It links out to the focused references: C interop, threading & memory, and HPC & SIMD.

- **[Contributing to cflat](doc/CONTRIBUTING.md)** - Building the compiler, development workflow, and running tests.
