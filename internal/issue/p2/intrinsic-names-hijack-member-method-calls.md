# Compiler intrinsic names hijack MEMBER calls: `x.is_string()` never calls the method

A member method whose name matches a compiler intrinsic (`is_string`, `is_pointer`, `is_unique`,
`is_interface`, `is_copyable`, `is_cpp_class`, `is_primitive`, ...) is silently replaced by the
intrinsic when called as `obj.name()`. The method is never called; the result is the intrinsic's
compile-time answer about the receiver's TYPE. No diagnostic. Affects C++ classes and plain CFlat
structs alike.

Found 2026-09-26 probing nlohmann/json: `j["s"].is_string()` is `false` for a string value
(`j["s"].type()` is 3 = `value_t::string`, `dump()` is `"x"`); `is_string` does not appear in the IR
at all. Any C++ library with an `is_string()` / `is_pointer()` style API is affected.

## Repro (standalone, ~1 s)

```cpp
// ist.h
#pragma once
namespace ist { struct V { int t = 3; V(int x) : t(x) {}
    bool is_string() const { return t == 3; } bool is_pointer() const { return true; } }; }
```

```cflat
import cpp "ist.h";
extern int printf(const char* fmt, ...);
struct CV { int t = 3; bool is_string() { return t == 3; } bool is_copyable() { return true; } };
extern int main()
{
    ist.V v = ist.V(3);
    CV c = default;
    printf("cxx is_string=%d is_pointer=%d  cflat is_string=%d is_copyable=%d\n",
        v.is_string() ? 1 : 0, v.is_pointer() ? 1 : 0, c.is_string() ? 1 : 0, c.is_copyable() ? 1 : 0);
    return (v.is_string() && v.is_pointer() && c.is_string() && c.is_copyable()) ? 0 : 1;
}
```

Master cfdbeca4 prints all zeros, exit 1. Want all ones, exit 0.

## Root cause

`MainListener_PostfixExpression.cpp` ~9067: `kIntrinsics` (`"is_pointer", "is_unique",
"is_interface", "is_copyable", "is_cpp_class", "is_primitive", "is_string", ...`) is checked by bare
function name; the per-intrinsic handlers (`functionName == "is_string"` ~4801, `is_unique` ~4610,
`is_copyable` ~4670) run for a member-call postfix too, treating the receiver as the intrinsic's
argument. In the json case the receiver expression `j["s"]` was also evaluated more than once
(three `operator[]` calls for two uses) - check that disappears with the fix.

## Fix direction

Intrinsics are free-function spellings (`is_string(x)` / `is_string<T>()`); a call with a receiver
(`a.b()`, `p->b()`) must go through normal member lookup and never reach the intrinsic handlers.
Gate the intrinsic path on "no receiver". Audit every entry of `kIntrinsics`
(`va_start`, `embed`, `reflect`, `__popcount`, ... - any can collide with a member name).

## Notes for the fixing agent

- Regression: extend an existing `Test/library/cpp_interop_*.h` fixture with the `ist.h` members and
  add legs to `Test/test_cpp_interop.cb`; add the CFlat-struct half to an existing native test that
  already exercises the intrinsics (grep `is_copyable(` in `Test/`). No new test files.
- Keep the free-function intrinsic forms green (their existing tests must still pass).
- Do NOT add nlohmann/json to the test run (maintainer, 2026-09-26: third-party libraries are not
  part of the suite yet). Local real-world check only: `scratch/probe3/j09b.cb`, see
  `scratch/probe3/run.sh` for flags (nlohmann headers are in
  `~/.cflat-compiler-deps/vcpkg_installed/arm64-osx/include`).
