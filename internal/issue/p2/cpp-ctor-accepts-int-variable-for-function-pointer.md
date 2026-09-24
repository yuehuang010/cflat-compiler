# C++ constructor call accepts an int variable for a function-pointer parameter

## Summary

A C++ constructor whose parameter is a function pointer accepts a non-constant `int` argument
and lowers it with `sext` + `inttoptr`. C++ rejects this (only a literal 0 is a null pointer
constant). A free function or member method with the same parameter correctly refuses the same
argument with "no overload ... matches". A nonzero value is then called as code at runtime.

## Repro

```cflat
import cpp "cpp_interop_fnptr.h";   // Test/library/cpp_interop_fnptr.h
extern int main()
{
    int z = 7;
    cppfnptr.Sink s = cppfnptr.Sink(z);   // explicit Sink(Cb f) : value(f ? f(4) : -1)
    return 0;
}
```

Measured 2026-09-24 on master 11a58ada (macOS arm64 Release): compiles, exit 0; the program
dies with exit 138 (bus error, calls address 7). With `int z = 0` it runs and yields -1.
IR: `%1 = sext i32 %0 to i64`, `%2 = inttoptr i64 %1 to ptr`, passed to `_ZN8cppfnptr4SinkC1EPFiiE`.
Neighbours measured: `cppfnptr.callback_present(z)` and `s.member(z)` are refused.

## Root cause

Not investigated. The constructor argument match/lowering path differs from the free/member
path in `LLVMBackend::CreateOverloadedFunctionCall` (which gates literal 0 through
`IsNullPointerConstantArgument` and refuses a non-constant int).

## Fix direction

Refuse a non-constant integer for a C++ function-pointer constructor parameter the way the
free/member path does; keep literal `0` and `nullptr` accepted (value -1 via `Sink(0)`,
test_cpp_interop.cb leg 3301).
