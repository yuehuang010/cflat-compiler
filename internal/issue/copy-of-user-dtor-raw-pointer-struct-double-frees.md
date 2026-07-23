# Copying a struct with a user destructor over a RAW pointer double-frees

Filed 2026-07-23. A value struct that owns a heap resource through a RAW pointer field freed by a
USER-written destructor (no `unique` marker, no `copy()`) is classified COPYABLE, so `dest = src`
and `T b = a;` shallow bit-copy it. Both the source and the copy then run the destructor over the
SAME pointer -> double-free. Found while investigating Part 5 STEP 2 of
`internal/plan/ownership-transparent-assignment.md` (STEP 2 does NOT fix this - the fault is at the
COPY classification, not the drop-old scatter STEP 2 touches).

## Repro (double-frees, exit via free() of an already-freed block)

```cflat
struct Res {
    int  id   = default;
    int* heap = default;                 // RAW owned pointer - not `unique`
    ~Res() { if (heap != nullptr) { free(heap); heap = nullptr; } }   // user dtor frees it
};
Res makeRes(int n) { Res r = default; r.id = n; r.heap = (int*)malloc(4); return r; }

extern int main() {
    Res original = makeRes(1);
    Res a = original;                    // SHALLOW copy: a.heap == original.heap
    return 0;                            // scope exit: ~a frees heap, ~original frees it AGAIN
}
```

`alias Res a = original;` double-frees identically - the `alias` keyword on a value-local
declaration does not engage the borrow-suppression machinery (that keys on the SOURCE being an
alias, `srcIsAlias`, not on the LHS qualifier), so it is just another copy. The double-free is NOT
alias-specific; plain `Res a = original;` is enough.

## Root cause

`LLVMBackend::IsCopyableType` (LLVMBackend.h ~3556) is the single predicate every copy path trusts
(decl-init `ParseDeclaration` via `ClassifyOwningAssignSource`, field-store
`RejectOwningValueCopyIntoField`, reassign, deref, and the Part 6 container-slot store). Its struct
rule is:

```cpp
if (HasCopyOverloadFor(base)) return true;
return !TypeOwnsUniquePointer(base);      // <-- only checks `unique`-marked pointers
```

`TypeOwnsUniquePointer` only fires on a `unique` pointer field. A plain `int* heap` is invisible to
it, so `IsCopyableType("Res")` returns true. The copy is then emitted by `EmitCopyableOwnerCopy`,
which for a struct with no real `copy()` falls back to the memberwise synth - a field-by-field
shallow copy that bit-copies the raw pointer while `Res`'s user dtor frees it on both instances.

The correct predicate ALREADY EXISTS for one context: `ClosureCaptureDeepCopyable` (LLVMBackend.h
~3591) rejects a struct with ANY raw pointer/view field unless it has a real `copy()`, with the
exact comment "the synth shallow-shares those while T's dtor frees them -> double-free; such a type
needs a hand-written copy() to be capturable." `IsCopyableType` never got the same guard - a hole
in THE FLIP (plan Part 4) / the local-on-local-is-copy restructure (commit df01cc1). Before those,
`Res b = a;` was a MOVE (source zeroed, single owner) and did not double-free.

## Fix direction

Tighten `IsCopyableType`'s struct arm so a type whose memberwise synth would shallow-share an OWNED
raw pointer is NOT copyable. Do NOT copy `ClosureCaptureDeepCopyable` verbatim: it rejects ANY raw
pointer field, which would wrongly demote the many BORROW-holding value structs (iterators, views,
spans) that have a raw pointer but NO user destructor and are safely shallow-copied. Narrow the
guard to the genuinely-unsafe shape:

    non-copyable when: no real copy()  AND  StructData.Destructor != nullptr (author wrote ~T)
                       AND  a field is a raw Pointer / ElemPointer / IsArrayView
    (keep the existing TypeOwnsUniquePointer leg as well)

A user destructor + a raw pointer + no copy() is the "owns a raw resource the synth cannot safely
duplicate" signature; without a user dtor the raw pointer is a borrow and shallow copy is fine.

## Impact / caution (why deferred, not hot-patched)

`IsCopyableType` is load-bearing and FLIP-adjacent (Part 4 required explicit maintainer sign-off).
Flipping affected structs copyable -> non-copyable changes diagnostics and codegen: `T b = a;` on
such a type would require `move` or a hand-written `.copy()`, and the `is_copyable` intrinsic result
changes (which container insert / `if const` sinks read). Enumerate the deltas FIRST (grep tests /
examples / core for value structs with a user dtor + raw pointer + no copy()), then land behind the
`Test/test_collection_leaks.cb` HeapAudit matrix with a new element kind covering this shape. A
mis-tightening turns a silent double-free into a spurious compile error on a legitimately-copyable
borrow-holder.

## Related
- `internal/plan/ownership-transparent-assignment.md` - Part 4 (THE FLIP) and Part 5 STEP 2.
- `ClosureCaptureDeepCopyable` (LLVMBackend.h) - the correct predicate for the closure-capture path.
