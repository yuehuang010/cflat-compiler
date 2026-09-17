# An integer literal given to a free-function or member `const int&` parameter is rejected

Found 2026-09-17 by the round-1 review of fix/cpp-ctor-ref-param (reviewer probe scratch/rv7.cb in that worktree, not preserved). Pre-existing on master; not touched by the ctor-ref fix, which covers constructors only.

## Repro

```
// header
struct S { int get(const int& x) { return x; } };
int freefn(const int& x);
```
`s.get(5)` / `freefn(5)` -> no matching overload; `-v` shows `[0] arg=i8 param=int*`. A CFlat integer literal reaches the free-function wrapper binding as i8, and the reference-parameter path (IsRvalueReferenceArgument sites in LLVMBackend_CInterop.cpp) has no scalar materialization for a const reference.

## Fix direction

Same shape as the constructor fix: for a `const T&` scalar parameter with a non-addressable argument, cast the value to the referent width and materialize into a frame alloca; non-const `T&` must still require an lvalue.

Suggested bucket: p3.
