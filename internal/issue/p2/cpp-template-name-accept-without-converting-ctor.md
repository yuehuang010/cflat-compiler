# C++ T x = T<other args>(...) accepted without a converting ctor (template-name accept)

## Repro

```cpp
// counts.hpp
namespace r3 {
template<class T, class F = void (*)(int*)> struct Count {
    int value;
    Count(int v) : value(v) {}
    ~Count() {}
};
template<class T, class F = int> struct SA {
    static_assert(sizeof(T) == 4, "SA needs a 4-byte T");
    T v;
    SA(int x) : v(x) {}
    SA(const SA& o) : v(o.v) {}
    ~SA() {}
};
}
```

```cflat
import cpp "counts.hpp";
extern int main()
{
    r3.Count<int, int> x = r3.Count<long, int>(7);   // clang: no viable conversion
    r3.SA<int> y = r3.SA<i64>(7);                     // clang: static_assert + no viable conversion
    return 0;
}
```

## Observed vs expected

Observed: both compile. `ForeignCxxConstructArgs` (cflat/MainListener_Declarations.cpp) accepts a
flat-angle initializer whose template NAME matches the declared type and constructs the DECLARED
specialization in place from the arguments (`Count<int,int>(7)`, `SA<int>(7)`), so the written
specialization is never checked for a conversion (and SA<i64>'s static_assert never fires).
Expected: refuse when clang refuses (no converting constructor from the written specialization),
and convert through the selected converting constructor when one exists (an unconstrained
`Conv(const Conv<U>&)` counts 1,0,0,1,2 under clang - written built, then converted - vs 1,0,0,0,1
here).

Constraint: the ordinary path cannot reach CONSTRAINED converting constructor templates
(enable_if / explicit(bool) / requires - every std one): `std.pair<i64,int> p = std.pair<int,int>(3,4)`
and `std.optional<i64> o = std.optional<int>(3)` must keep working (S7 round 4 broke them by refusing;
round 5 restored the template-name accept, see Test/test_cpp_interop_template.cb leg 2825). A fix
must route through clang's overload resolution (wrapper) rather than refuse.

## Suspected area

`ForeignCxxConstructArgs` template-name accept (`base == MangledBase(typeName)`), present since
2026-09-07. Nested-angle callees with a different canonical spelling already take the ordinary path.
Found in S7 review rounds 3-4 (scratch/rv4/b_sa.cb, scratch/rv/r3_flatwrong.cb).
