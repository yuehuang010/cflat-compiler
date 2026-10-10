# C++ header namespace expression statements still crash Clang Sema

## Repro

In a C++ header, place any of these in a namespace after declarations:

```cpp
namespace n { int x, y; x < y; }
namespace n { int x, y; x > y; }
namespace n { int x; x << 1; }
namespace n { int x; decltype(x)(1); }
namespace n { int x; int(x) + 1; }
```

Both the pre-T66 master executable and the T66 build SIGSEGV (rc 139).
Clang's C++20 syntax oracle diagnoses the same inputs with rc 1.

## Root cause and fix direction

The token predictor in `CxxIncrementalGroup.cpp` cannot safely identify these as expression
statements. `<`, `>`, and `<<` overlap template-argument parsing; `decltype(x)(1)` and
`int(x) + 1` look declaration-like. Predicting them as expressions also misclassifies malformed
declarations such as `int broken(` and can hang error recovery. Add a Sema-side guard or detect
`ParseTopLevelStmtDecl` in namespace context and diagnose there instead of broadening token
prediction.
