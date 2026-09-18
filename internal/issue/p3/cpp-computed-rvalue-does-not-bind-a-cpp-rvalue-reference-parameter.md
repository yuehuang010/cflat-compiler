p3

# A computed rvalue (`a + 1`) does not bind a C++ `T&&` parameter, so a `const T&` overload wins instead

Found 2026-09-17 as round-3 finding (3) of fix/cpp-const-ref-scalar, and ruled out of that fix's
scope because closing it changes a CFlat-wide rule, not the const-ref binding it was fixing.

## Repro

```cpp
// header
namespace crs {
    inline int only_rv(int&& v)   { return 200 + v; }
    inline int mixr(const int& v) { return 100 + v; }
    inline int mixr(int&& v)      { return 200 + v; }
}
```

```cflat
import cpp "crs.h";
extern int main()
{
    int a = 40;
    printf("%d\n", crs.only_rv(a + 1));   // error, see below
    printf("%d\n", crs.mixr(a + 1));      // 141 - the COPY leg; C++ picks 241
    return 0;
}
```

A lone `int&&` candidate refuses the argument outright:

```
parameter 'v' of 'crs.only_rv' takes ownership of the value; pass 'move <arg>' or a temporary value
```

`only_rv(41)` and `only_rv(move a)` both return 241, and `mixr(41)` / `mixr(move a)` both pick the
move leg. Only a COMPUTED rvalue misses.

## Root cause

`IsRvalueReferenceArgument` (`LLVMBackend_Lookup.cpp:474`) is
`arg.IsExplicitMove || IsConsumableTemporary(arg)`. A literal is a consumable temporary; the result
of `a + 1` is not. The overload loop breaks out of a `T&&` candidate whenever
`IsRvalueReferenceArgument` is false, so the candidate is gone before any ranking runs and no
tie-break can reach it.

## Why it is not part of the const-ref fix

On master this exact call exits 139 (the const-ref leg was the only binder and the raw scalar was
passed as an address). fix/cpp-const-ref-scalar makes it return the correct value for the leg CFlat
admits, so that branch is a strict improvement here; it simply cannot also change WHICH leg binds.

## Fix direction

Widen what counts as an rvalue for `T&&` binding to include a non-addressable computed value (an
arithmetic expression, a call result already counts as a temporary in some paths - check which).
`IsConsumableTemporary` also drives `ScoreMoveAgreement` and the "takes ownership of the value"
diagnostic family, so this needs its own accept-set first: every `move` parameter and every `T&&`
overload reached with an expression argument in `core/`, `Test/` and `example/`, frozen as value
legs BEFORE the predicate is widened. A wrong widening here silently consumes a caller's variable.
