# `P<int*[]>` collapses into the `P<int[]>` instantiation - element star dropped from the type-argument key

Bucket: full mode (moves instantiation keys), not batchable. Filed 2026-09-04 by the q09 review.

## Summary

The type-argument encoder in cflat/MainListener_Declarations.cpp ~214-224 spells a view argument
as `if (hasArrayView) resolved += "[]"; else if (hasPointer) resolved += stars;`, so the element
star of `int*[]` is silently dropped: `P<int*[]>` keys as `P$.v$int`, the same instantiation as
`P<int[]>`, `typeof(h)` prints `P<int[]>`, and a `T*` field becomes `int*` instead of `int**`.

## Repro (master e0afc3b3)

```
struct P<T> { T* p = default; };
extern int main() {
    int a = 1, b = 2;
    int*[] fixed = new int*[2]; fixed[0] = &a; fixed[1] = &b;
    P<int*[]> h; h.p = fixed;
    return *h.p[1];    // module verifier dump "Load operand must be a pointer", no LogError
}
```

`P<IP[]>` with `using IP = int*;` works only because the alias route re-adds depth through
`aliasPtrDepth`.

## Fix direction

Carry the element pointer depth in the argument encoding (`int*[]` must key differently from
`int[]`) and in the substituted `T x` view-of-pointers repr, which also loses `ElemPointer`. The
q09 decay arm (`substArgIsArrayView` in ParseDeclarationSpecifiers) is downstream and already
correct once the substitution string carries the star; the review measured that patching the arm
alone changes nothing. Add a LogError for the verifier dump either way. Accept-set must include
every existing `P<int[]>` / `P<T*>` instantiation pair (keys stay distinct, values unchanged).
