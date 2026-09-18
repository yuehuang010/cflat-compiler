# A C++ namespace alias declared INSIDE a namespace does not resolve namespace-scope objects

Bucket: p3 (compile error on a legal spelling; the function/type spellings work).

## Summary

`namespace outer { namespace sh = inner; }` is harvested correctly by the C++ namespace-alias
binding (landed 2026-09-17), and the FUNCTION spelling through it works. The namespace-scope
OBJECT spelling does not:

```
rev2_ns.cb(5,8): 'inner_obj' is not a member of namespace 'rt_outer.sh'.
```

A TOP-LEVEL alias (`namespace rt_top = rt_inner;`) resolves every member kind, objects included -
that is the shape the landed fix covers and pins (legs 1970-1976 in `Test/test_cpp_interop.cb`).
Only the nested-declaration shape is left.

## Repro

```cpp
// rev2_ns.h
namespace rt_inner
{
    inline long inner_obj = 51;
    inline long inner_fn() noexcept { return 53; }
}
namespace rt_outer { namespace sh = rt_inner; }
namespace rt_top = rt_inner;
```

```cflat
import cpp "rev2_ns.h";
extern int main()
{
    if (rt_top.inner_obj != 51) return 1;          // OK
    if (rt_outer.sh.inner_fn() != 53) return 3;    // OK
    if (rt_outer.sh.inner_obj != 51) return 2;     // error above
    return 0;
}
```

Same result cold and warm (`CFLAT_CACHE_DIR` reused), so the alias pair IS in the disk cache; the
failure is at lookup, not at harvest.

## Root cause

`LLVMBackend::IsCxxNamespace` (`LLVMBackend_CInterop.cpp:4840`) tests only the LEADING dotted
segment against `cxxForeignNamespaces_`. For the alias key `rt_outer.sh` the lead `rt_outer` is a
real C++ namespace, so `qualifiedCxxNamespace` is true at
`MainListener_PostfixExpression.cpp:1517`, and that branch deliberately keeps the EXACT spelling
instead of calling `ResolveNamespace` (it must, so a using-directive rewrite does not fire on a
plain C++ namespace path). The alias therefore never hops: `namespaceContext` stays
`rt_outer.sh`, and the object lookup asks for `rt_outer.sh.inner_obj`, which no global is
registered under. The function spelling survives because it falls through to clang's own
on-demand lookup, where `rt_outer::sh::inner_fn` is valid C++.

## Fix direction

At `MainListener_PostfixExpression.cpp:1517`, hop an EXACT `namespaceAliasTable` entry even when
`IsCxxNamespace(qualifiedName)` is true - i.e. a new narrow accessor that looks the whole dotted
name up in the alias table (plus the `stackNamedVariable` alias frames) and returns the name
unchanged when there is no entry, so a C++ namespace with no alias keeps today's behaviour and no
using-directive rewrite is introduced. That is a firing-condition change on a name-resolution
guard every C++ namespace walk crosses, so it needs its own accept-set (plain C++ namespace path,
a using-directive-nominated path, a CFlat `namespace a = b;` whose lead is a foreign namespace)
rather than a one-site edit.
