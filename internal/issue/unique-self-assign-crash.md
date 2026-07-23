# `unique R*` self-assign `a = a;` compiles and segfaults at runtime

Opened 2026-07-22 during the assignment-semantics probe for
`internal/plan/ownership-transparent-assignment.md`.

## Summary

Self-assigning a `unique R*` local compiles cleanly and crashes with signal 11 at the next
dereference. Classic self-move: the assignment path marks the source moved / zeroes it, so
the variable dereferences null (or freed memory) afterward. The aggregate owners are fine -
`Holder`/`Row`/`CRow`/`list<int>` and closure self-assign are all sound (the closure path
clones the source env BEFORE freeing the old dest, and the struct destruct-old block has an
explicit `destination != rightNV.Storage` self-assign guard at MainListener.h ~9868). The
thin unique-pointer path has no such guard.

## Repro (signal 11; expected: no-op or compile error)

```cflat
import "diagnostic/heap_audit.cb";
struct R { int v = 0; };
extern int main()
{
    HeapAudit.enable();
    {
        unique R* a = new R();
        a->v = 5;
        a = a;                      // self-move
        printf("a.v=%d\n", a->v);   // segfault
    }
    if (HeapAudit.reportLeaks() != 0u) return 10;
    return 0;
}
```

## Root cause

The owning-move reassign branch in `ParseAssignmentExpression` (MainListener.h ~9790-9818)
consumes the source (zero + MarkVariableMoved) without checking `destination == source`.
For `a = a` the source and destination are the same alloca, so the store is preceded (or
followed) by zeroing the very slot being assigned.

## Fix direction

Two acceptable outcomes; pick one in Part 3 of the plan:
1. Compile error: "self-assignment of a unique local is a no-op / not allowed" (cheapest,
   matches the spirit of the existing unique diagnostics), or
2. Runtime no-op: skip the consume when source storage == destination storage, mirroring
   the `destination != rightNV.Storage` guard the struct block already has.
Either way add the guard where the thin-pointer move branch fires, and extend
`Test/errors/` or the leak matrix accordingly.
