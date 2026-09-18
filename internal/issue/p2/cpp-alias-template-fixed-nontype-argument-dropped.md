# A C++ alias template that FIXES a non-type argument silently yields the default instantiation

Bucket: p2 (silent wrong value, no diagnostic).

## Summary

`template <class T> using NBdef = NBox<T, 7>;` names `NBox<T, 7>`. CFlat resolves the alias by
hopping to the TARGET BASE (`NBox`) and then re-mangling with only the type arguments spelled at
the use site, so the alias's own fixed argument `7` is dropped and the DEFAULT instantiation
(`NBox<int, 4>`) is bound instead. No diagnostic; the program runs with the wrong value.

Affects the DECLARATION spelling on master and, since it routes through the same hop, the
CONSTRUCTOR spelling landed with the alias-template constructor fix. Found in review of that fix
(2026-09-17) by probing the non-type-parameter axis.

## Repro

```cpp
// rev2_tpl.h
namespace r3 { template <class T, int N = 4> struct NBox { T v; NBox() noexcept : v(T(N)) {} }; }
namespace r3a { template <class T> using NBdef = r3::NBox<T, 7>; }
```

```cflat
import cpp "rev2_tpl.h";
extern int main()
{
    r3a.NBdef<int> n7 = default;
    printf("decl=%d\n", n7.v);                        // prints 4, must print 7
    r3a.NBdef<int> n7b = r3a.NBdef<int>();
    printf("ctor=%d\n", n7b.v);                       // prints 4, must print 7
    return 0;
}
```

Measured: master (97773655) prints `decl=4` and refuses the constructor spelling
("the function 'NBdef' is not known"); the batch-cpp-diag binary prints `decl=4 ctor=4`. Correct
is 7 in both. An alias that leaves the non-type parameter free (`using NB = NBox<T, N>;`) is
correct - `r3a.NB<int>()` prints 4 because 4 is genuinely the default.

## Root cause

The alias hop is BASE-ONLY: `IsGenericBaseAlias` / `ResolveGenericBaseAlias` map the alias name to
the target's base name and the caller then re-mangles with the use site's type arguments
(`MangleGenericInstance(aliasBase, typeArgs)`), e.g. in
`MainListener_PostfixExpression.cpp` (both alias arms), `MainListener_Declarations.cpp:893/2078`
and `ForwardRefScanner.cpp:1788`. The alias's ARGUMENT PATTERN (`<T, 7>`) is recorded in
`RawTypedef::cxxAliasPattern` but is not consulted on this path, so every fixed or reordered
argument in the pattern is lost.

## Fix direction

Resolve an alias template through its recorded PATTERN rather than its base: substitute the use
site's arguments into `cxxAliasPattern` positionally and request the specialization the
substituted pattern names. Needs its own accept-set - the pattern axis includes a reordered
pattern (`using Flip = Pair<B, A>;`), a partially fixed pattern, and an alias whose pattern names
another alias - and it touches every `ResolveGenericBaseAlias` caller listed above, so it is not a
one-site change. Interacts with
`internal/issue/p3/cpp-alias-template-unqualified-target-unresolvable.md`, which is the other
defect in the same stored pattern.
