# C++ callback-parameter overloads: function-pointer values lose pointee const; clang-refused sets accepted

Two pre-existing gaps left after T25 (landed 33866560, 2026-10-03). T25 made named functions (CFlat and
imported C/C++) carry pointee const into callback-overload ranking; everything else declines to master's
ranking.

1. A function-pointer VALUE (auto variable, typedef'd pointer like `CompareC`, closure) passed to a C++
   overload set `pick(int(*)(const void*, const void*))` / `pick(int(*)(void*, void*))`: CFlat picks the
   const overload for both a const and a mutable callback (1 1); clang picks 1 2. The function-pointer
   value's CFlat type does not carry pointee const.
2. Sets clang REFUSES (no viable candidate) are accepted: `pick(int(*)(const int*))` /
   `pick(int(*)(void*))` called with an `int(*)(int*)` callback (CFlat 61), and
   `pick(int*(*)(const int*))` / `pick(const int*(*)(int*))` with `int*(*)(int*)` (CFlat 71).

## Repro

`scratch/repro_keep/t25_gaps/` - rev_t25_r3_ret.{hpp,cb,cpp} (case 2, prints 71; clang refuses) and
rev_t25_scope2.* (case 2, prints 61). Case 1 shape:

```cflat
import cpp "t25_csurface.hpp";   // Test/library: pick(C)->1, pick(M)->2 over const/mutable void* callbacks
extern int main()
{
    auto f = t25surface.cb_mutable;
    return t25surface.pick(f) == 2 ? 0 : 1;   // clang 2; CFlat 1
}
```

## Fix direction

1. Keep pointee const in the CFlat function-pointer type (signature of a function-pointer value), then
   drop the "named function only" decline in preferCallbackConstSpelling.
2. Function-pointer argument conversion must follow C++: no implicit pointee-const conversion inside a
   function-pointer type; refuse when no candidate is viable.
