# Debug C++ template error check reaches an Itanium mangler unreachable

## Summary

On the macOS arm64 Debug build linked against assertions-enabled LLVM 23.1, one C++ error
fixture aborts inside Clang's Itanium mangler instead of reporting its expected diagnostic:

```
unexpected statement kind
UNREACHABLE executed at .../clang/lib/AST/ItaniumMangle.cpp:5011!
```

This is distinct from the trivial-destructor harvest issue and from the invalid constant-expression
cast assertion observed in `Test/test_cpp_interop.cb`.

## Repro

From the repository root after `./cmake_build.sh debug` and `x64/Debug/cflat --init-local`:

```
x64/Debug/cflat --check -i Test/library Test/errors/err_cpp_struct_tpl_arg_incomplete_deque.cb
```

Observed exit code: 139. The output first says `unexpected statement kind`, then reports the
unreachable at `clang/lib/AST/ItaniumMangle.cpp:5011`.

## Root cause

Unknown. lldb cannot launch processes in the current sandbox (`process exited with status -1`),
so there is no native backtrace.

## Fix direction

Reproduce with a working Debugger and identify which mangling request receives the unexpected
statement. Keep the LLVM/Clang unreachable assertion enabled.
