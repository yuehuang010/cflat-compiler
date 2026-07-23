# Storing a named `unique R*` local into a `unique R*` struct field silently aliases

Opened 2026-07-22 during the assignment-semantics probe for
`internal/plan/ownership-transparent-assignment.md`.

## Summary

`w.f = a` where both `w.f` and `a` are `unique R*` compiles with NO diagnostic and produces
a genuine alias: both handles stay live and point at the same object, and `a` is NOT marked
moved. This is exactly the shape the X4 guard rejects for owning STRUCT values ("copying
owning value 'Holder' by value into a struct field aliases its backing buffer and will
double-free") - but the X4 predicate is aggregate-based (`IsOwningValueType`), so a
pointer-typed field falls through to a plain store. Use-after-free is one perturbation away
(verified: reassigning the field frees the shared pointee while `a` still reads it).

## Repro (compiles clean; UAF demonstrated)

```cflat
import "diagnostic/heap_audit.cb";
struct R { int v = 0; };
struct Wrap { unique R* f = default; };
extern int main()
{
    HeapAudit.enable();
    Wrap w = default;
    unique R* a = new R();
    a->v = 5;
    w.f = a;                     // no guard: silent alias, a NOT consumed
    printf("%d\n", (int)(a == w.f));  // prints 1
    w.f = new R();               // frees old f == a's pointee (EmitUniqueFieldDelete)
    printf("%d\n", a->v);        // use-after-free: reads freed memory (prints 0)
    return 0;
}
```

Also note the teardown accounting in the simple (no-perturbation) case exits 0, i.e. exactly
one free happens for two owners - one of the two release paths is silently skipped, which is
accidental, not designed.

## Root cause

The X4 field-store guard (`MainListener.h:9580`, `RejectFieldAllocAlignMismatch`) and the
string field-store guard key on owning STRUCT/string types; `unique T*` field destinations
are handled by a different mechanism (`EmitUniqueFieldDelete` frees the OLD pointee on
reassign) that never validates or consumes the SOURCE. So the source-side ownership transfer
is simply missing for pointer-typed fields.

## Fix direction

Make the unique field store a real transfer (Part 2/3 of the plan): consume the named
source (`MarkVariableMoved` + null it) exactly as the by-value move-sink does, or - interim,
matching current init strictness - reject the plain form and require `w.f = move a;`.
Contrast with `unique <interface>` fields, which are OVER-strict today
(`internal/issue/unique-interface-move-rejected.md`); the fix should unify both on one
predicate.
