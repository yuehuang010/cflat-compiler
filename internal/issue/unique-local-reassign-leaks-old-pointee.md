# `unique R*` local reassign over a live value leaks the old pointee

Opened 2026-07-22 during the assignment-semantics probe for
`internal/plan/ownership-transparent-assignment.md`.

## Summary

Reassigning a live `unique R*` local from another `unique R*` local compiles WITHOUT `move`,
is classified as a move (subsequent reads of the source are rejected with "use of moved
variable"), but the pointee the destination previously owned is NEVER freed -> leak.
This is inconsistent with init, which rejects the same shape outright
("cannot initialize unique ... from a borrowed value").

## Repro (exit 10 under HeapAudit; expected 0)

```cflat
import "diagnostic/heap_audit.cb";
struct R { int v = 0; };
extern int main()
{
    HeapAudit.enable();
    {
        unique R* a = new R();
        unique R* b = new R();
        b = a;              // compiles; a is marked moved; b's OLD pointee is never freed
    }
    if (HeapAudit.reportLeaks() != 0u) return 10;   // -> 10 (leak)
    return 0;
}
```

## Root cause

`ParseAssignmentExpression` (MainListener.h 9107-9983) has destruct-old-destination logic
scattered across five type-specific blocks (9528, 9595, 9857, 9880, 9901), and none of them
covers a thin `unique T*` LOCAL destination: the owning-value block at 9857-9878 keys on
`IsOwningValueType` (structs), the string block on `TypeName == "string"`, and
`EmitUniqueFieldDelete` (9880-9899) fires only for a struct FIELD destination. A unique
LOCAL reassign from a named source falls through to the plain `derefAssign` store at ~9918
with no old-pointee free. Note the compiler's own diagnostic elsewhere promises the opposite:
"reassigning it frees the current object first" (the cannot-delete-unique-local message).

## Fix direction

Route reassign through a single total DropValue(dest) before the store (Part 2 of
`internal/plan/ownership-transparent-assignment.md`): for a live `unique T*` destination,
free the old pointee exactly as scope-exit would, then store. Alternatively (minimal fix)
add the missing unique-local case next to the string-local block at 9901. Init/reassign
asymmetry (init rejects, reassign accepts) should also be unified - see the plan's Part 3.
