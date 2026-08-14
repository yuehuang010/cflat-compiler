# Unaligned move into an `alignas`-declared local is silently accepted (field store already rejects it)

Filed 2026-08-13, found while fixing the `test_core` heap-corruption crash (see
[[centralize-scoped-registry-resolution]] area / `SetVariableAllocAlignment`,
`cflat/LLVMBackend_OwnershipTemps.cpp`). Not a correctness bug on its own - the crash fix's
`SetVariableAllocAlignment` is deliberately monotonic (`std::max`, no-op on 0), so this hole cannot
regress a currently-correct free. It is a missing diagnostic: the declared clause on the local
becomes a lie with no error.

## Repro (compiles clean, should probably be rejected)

```cflat
struct Elem { int id; };
extern int main()
{
    unique alignas(0, 64) Elem* values;
    unique Elem* other = new Elem[2];
    values = move other;          // accepted; declared clause is now a lie
    return 0;
}
```

`values` is declared with an explicit allocation-alignment clause
(`TypeAndValue.AllocAlignValue == 64`), but `other` was allocated with the ordinary allocator (no
clause). The move is accepted with no diagnostic. Because `SetVariableAllocAlignment`
(`cflat/MainListener_Expressions.cpp:3233`) is a monotonic no-op on a 0-alignment source, `values`
keeps whatever alignment it already had recorded (0 here, since it was declared without an
initializer) and frees correctly through the plain deallocator - so this specific repro does not
crash. But the declared-type clause is now meaningless: nothing checks the RHS against it, and a
different sequence (e.g. the local already holding an over-aligned block, then reassigned from an
ordinary one) would silently downgrade tracked alignment away from what the type promises, which is
exactly the shape of bug the field-store path was hardened against.

## The asymmetry with fields

`MainListener_Declarations.cpp:5470`, `RejectFieldAllocAlignMismatch`, already rejects the
equivalent case for a STRUCT FIELD: storing a pointer into a field whose type carries an
`alignas(0,N)` clause is checked against the source's actual `AllocAlignment`, with three distinct
error messages depending on which side has the clause. See that function's block comment for the
reasoning (an over-aligned pointee TYPE is exempt since `new`/`delete` both re-derive from the
static type; an `IsArrayView` fresh buffer or any non-null pointer into a clause-bearing `unique`
scalar field is checked).

Local-variable assignment (`MainListener_Expressions.cpp:3211-3234`, the block that calls
`SetVariableRawNewArray` / `SetVariableAllocAlignment` on reassignment) has no analogous check. It
re-derives the runtime `AllocAlignment` from the RHS unconditionally and never compares it against
`namedVar.TypeAndValue.AllocAlignValue` when the local's OWN declared type carries a clause.

## Why not fixed in the same change

The crash fix (`SetVariableAllocAlignment`) needed to be safe and monotonic first, to stop the
heap corruption without touching validation semantics - see the two related runtime-state fixes
this landed alongside: [[move-pointer-parameter-loses-raw-array-count]] and
[[temporary-raw-array-join-loses-count-and-ownership]] (paths deleted alongside the fix; see
`Test/test_core.cb`'s `runRawCountJoin*` / `runRawCountMove*` block for the full accept-set that
had to stay green). Per `internal/fix-issue-lessons.md`, a new rejection needs its own
must-still-work accept-set built and measured BEFORE landing, the same way the `RejectField...`
check above documents three separate accepted/rejected shapes in its comment. That measurement
was out of scope for a crash fix.

## Fix direction

Mirror `RejectFieldAllocAlignMismatch` for the local-reassignment door in
`MainListener_Expressions.cpp` around line 3230: when `namedVar.TypeAndValue.AllocAlignValue > 0`
(the local's OWN declared type carries a clause - not the inferred/tracked
`NamedVariable.AllocAlignment`, which is the mutable runtime side-channel, not a source of truth),
compare it against `rightNV.AllocAlignment` the same way the field path does, with the same three
message shapes (field has no clause but RHS is over-aligned / field has a clause but RHS is not /
both have clauses but disagree - adapted to "local" wording). Needs a measured accept-set first:
run against every `runRawCountJoin*` / `runRawCountMove*` / `runRawCountConditionalMove` case in
`Test/test_core.cb` (added by the two commits this crash fix followed:
"Fix raw array count runtime state", "Fix raw-array provenance across joins and moves") plus the
existing `RejectFieldAllocAlignMismatch` field-side test coverage, since some of those cases
intentionally move/join a non-clause-bearing local into one - confirm which are legitimate borrows
of a scalar `new T` (which the field check exempts via `!fieldTV.IsArrayView` reasoning) versus
which are the genuine bug this issue describes.

## Related
- `cflat/LLVMBackend_OwnershipTemps.cpp:935` - `SetVariableAllocAlignment` (the crash fix; explicitly
  documents itself as monotonic/no-op-on-0 and does not attempt this validation).
- `cflat/MainListener_Declarations.cpp:5470` - `RejectFieldAllocAlignMismatch` (the field-side check
  to mirror).
- `internal/fix-issue-lessons.md` - accept-set-before-rejection lesson.
