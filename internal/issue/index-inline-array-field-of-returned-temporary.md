# Plain READ of an inline-array field of a returned temporary is rejected, should work

Filed 2026-07-24. Deliberate scope cut from the crash fix that diagnosed (instead of crashing
on) indexing an inline array field of a temporary. Not a regression: this shape ALWAYS crashed
the compiler; it now errors instead.

## Repro

```cflat
struct Pack { int vals[4]; }
Pack mk() { Pack p = default; p.vals[0] = 7; return p; }

int main()
{
    int x = mk().vals[0];   // error: "'[]' requires an addressable source ..."
    return x;
}
```

Expected: compiles, x == 7 (C and C++ both allow this; in C99 the returned temporary has
automatic storage duration to the end of the full expression). Actual: LogError from the
Storage==nullptr guard in ParsePostfixExpression (cflat/MainListener.h, subscript lowering,
search for "requires an addressable source"). Workaround is the message's suggestion:
`Pack p = mk(); int x = p.vals[0];`.

## Root cause

A field read off a by-value temp is a CreateExtractValue - a register value with null Storage.
The inline-array subscript branch needs an address to GEP, so before the fix it built the GEP
on a null base and crashed in llvm::ConstantFolder::FoldGEP. The guard rejects instead of
materializing storage.

## Fix direction

Spill instead of reject, for the READ form only. Precedent lives ~100 lines above the guard:
the operator[] dispatch spills a storage-less struct value (CreateAlloca + CreateAssignment,
then Storage = temp). In the guard branch, when the named value's Primary is non-null, alloca
the extracted array type, store Primary, set Storage to the spill, and fall through to the
existing GEP branches.

Two traps to design for, which is why this was not folded into the crash fix:

1. `move mk().vals[i]` must STAY rejected. ParseMoveExpression's addressability guard keys on
   Storage == nullptr; after the spill Storage is non-null, so move would null a slot in the
   spilled SHALLOW COPY while the original temp still holds the pointer (leak or double free).
   Needs an explicit "storage is a temp spill" marker on NamedVariable that the move path
   checks. If the marker ends up on TypeAndValue/StructData, remember the --init serializer
   round-trip rule in CLAUDE.md.
2. Owning element types: the parent temp's end-of-full-expression destruction
   (MoveTempStructAlloca machinery, LLVMBackend.h) must not double-run against the partial
   spill, and a borrowed element read must not outlive the full expression any more than a
   whole-field read (`mk().item`) already can - keep the two forms at parity.

Regression home when fixed: extend an existing Test/ file exercising inline-array fields (do
not create a new test file), plus keep the move-form expect_error leg in
Test/errors/err_move_field_borrowed_param.cb passing unchanged. Note that the same file's
bare-READ and STORE legs (the two `expect_error` legs pinned on "inline array field of a
temporary" that are not the `move` form) must be RETIRED by that fix - spilling for the read
form makes those shapes legal, so they will go red by design.
