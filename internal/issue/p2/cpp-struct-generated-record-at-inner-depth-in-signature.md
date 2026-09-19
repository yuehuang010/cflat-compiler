# Bucket p2. A `[cpp]` struct POINTER at inner depth binds as a local but not in a file-scope signature

Found 2026-09-19 while fixing
internal/issue/p2/cpp-nested-user-class-pointer-at-inner-depth-unresolvable.md
(fix/cpp-nested-user-ptr, macOS arm64, Release). Measured IDENTICAL on master 7f6db1ed and on that
branch, in every spelling below - not caused by that fix, and a DIFFERENT root cause from it.

## Summary

With `[cpp] struct BP`, `std.vector<std.vector<BP*>>` compiles and runs as a LOCAL, as a CFlat
struct FIELD's initializer and through `push_back`, but the same spelling in a FILE-SCOPE
SIGNATURE (parameter or return type) is refused with

    cannot find the type 'std.vector<std.vector<BP*>>'

The refusal comes from the ForwardRefScanner signature pre-pass: the outer specialization is never
requested when an argument names a GENERATED (`__cflat_user::`) record. It is order-independent -
a function declared EARLIER that uses the identical local still does not satisfy it
(scratch/nup_p5_sigorder.cb). The equivalent spelling over a plain imported C++ class
(`std.vector<std.vector<nest.Cell*>>`) DOES work in a signature - fixture leg 2409 - so this is
specific to generated records.

Depth 1 in a signature works: `int bp_take1(std.vector<BP*> r)` binds and runs.

## Repro

scratch/nup_p2_bridgesig.cb, scratch/nup_p5_sigorder.cb in the fix worktree:

```
import cpp "vector";
[cpp] struct BP { int v = 5; int get() { return v; } };
int bp_take(std.vector<std.vector<BP*>> g) { return (int)g.size(); }   // refused
extern int main()
{
    BP one = default;
    std.vector<BP*> row = default;
    row.push_back(&one);
    std.vector<std.vector<BP*>> grid = default;     // this binds and runs
    grid.push_back(row);
    return bp_take(grid);
}
```

## Also measured (same probe family, NOT this issue)

`std.vector<std.vector<BP>>` - the by-VALUE form - is refused even as a LOCAL
(`no imported C++ header declares 'std::vector' - tried 'vector'`; the inner
`std::vector<__cflat_user::BP>` instantiation reports errors and its bodies are emptied). That is
the `[cpp]` struct by-value-in-containers item already held by maintainer ruling, not this one.

## Fix direction

Start in the ForwardRefScanner signature path that the sibling issue
`cpp-nested-std-specialization-in-file-scope-signature-still-unresolved` fixed for imported C++
classes: it now requests every nested component before the outer one. Find why that pre-pass does
not reach the same conclusion when the nested component is a generated record (likely the
generated definition/forward declaration is not available that early, so the component request is
skipped instead of deferred). Accept set to freeze first: depth 1 in a signature, and every local
/ field form at depth 2, which all work today.

## Related audit note

`GeneratedCxxDefinitionsFor`'s `visit` (cflat/LLVMBackend_CInterop.cpp ~3911) walks a mangled
argument by stripping a TRAILING `*` and recursing into template ARGUMENTS, so it misses a
pointer-prefixed (`.p$BP`) generated record at inner depth - the same shape fixed in
`CollectCxxTypeOwnerGroups` on 2026-09-19. It is currently UNOBSERVABLE: when it returns nothing,
`RequestCxxForeignType` falls back to `GeneratedCxxPrefixForSpelling`, which substring-matches
`__cflat_user::` in the C++ SPELLING and supplies the same definitions. No probe could make it
fail, which is why it was not changed. If that fallback is ever narrowed, fix the walk in the same
change.
