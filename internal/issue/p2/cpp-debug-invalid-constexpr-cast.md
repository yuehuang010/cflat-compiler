# C++ interop Debug asserts on an invalid constant-expression cast

## Summary

On the macOS arm64 Debug build linked against the assertions-enabled LLVM 23.1 tree,
checking the full C++ interop program aborts in LLVM's `Constants.cpp`:

```
Assertion failed: (CastInst::castIsValid(opc, C, Ty) && "Invalid constantexpr cast!"), function getCast, file Constants.cpp, line 2376.
```

This is separate from `cpp-header-harvest-emits-trivial-dtor-debug-assert.md`: the latter's
minimal importer, template test, and bridge test all pass in this worktree.

## Repro

From the repository root after `./cmake_build.sh debug` and `x64/Debug/cflat --init-local`:

```
x64/Debug/cflat --check -i Test/library Test/test_cpp_interop.cb
```

Observed exit code: 134. The compiler state dump reports `Test/test_cpp_interop.cb`, line
4115, column 12. The assertion occurs during this full interop check.

## Root cause

Unknown. `lldb --batch -o run -o 'bt 25' -- x64/Debug/cflat --check -i Test/library Test/test_cpp_interop.cb`
could not launch the process in the current sandbox (`process exited with status -1`), so no
backtrace was available. Do not infer that the reported CFlat source location identifies the
invalid cast site.

## Fix direction

Reproduce with a working Debugger, capture the LLVM/Clang stack, and minimize the C++ interop
operation that constructs the invalid `ConstantExpr` cast. Keep the LLVM assertion enabled.
