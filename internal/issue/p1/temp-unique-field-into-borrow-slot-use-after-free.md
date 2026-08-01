# A temp's `unique` field bound into a NON-owning slot is a silent use-after-free

Filed 2026-08-01 by the round-1 review of `fix/uniq-family`. **Pre-existing** - measured
identical on master `cf5e909` and on that branch. That branch closed the case where the
DESTINATION is itself a `unique` field (two owners) and the case where the pointee has a user
DESTRUCTOR (which used to crash the compiler); this residual case, a dtor-less pointee stored
into a plain borrowing slot, is untouched by it and was left open deliberately.

Severity: silent use-after-free. The program compiles clean, runs, prints a plausible value and
exits 0 - there is no abort to notice and no diagnostic. That makes it quieter than the exit-134
family, not milder.

## Repro

```cflat
struct Node { int v = default; };
struct Box<T> { T t = default; };
struct PlainSlot { Node* p = nullptr; };
Box<unique Node*> makeBox() { Box<unique Node*> b = default; b.t = new Node(); b.t->v = 70; return b; }
extern int main()
{
    PlainSlot q = default;
    q.p = makeBox().t;
    printf("v=%d\n", q.p->v);
    return 0;
}
```
```
v=70
```
compile rc 0, run rc 0 on both binaries. The 70 is read out of freed memory.

## Confirmed by the emitted IR, not inferred from the shape

```llvm
%1 = call %Box__unique_Nodeptr @_makeBox_Box__unique_Nodeptr__()
%2 = extractvalue %Box__unique_Nodeptr %1, 0
%owntemp = alloca %Box__unique_Nodeptr, align 8
store ptr %.fca.0.extract, ptr %owntemp, align 8
call void @Box__unique_Nodeptr.dtorfull(ptr nonnull %owntemp)   ; frees the Node
%3 = load i32, ptr %2, align 4                                  ; then reads it
```

The temp `Box` is registered as an owned struct temp and destructed at the end of the statement,
which frees the `Node`; the `printf` argument is loaded from that same pointer afterwards. The
store into `q.p` is optimized away entirely, so the slot is not even the thing that dangles -
the value is.

## Why the existing gates miss it

The two rejects that DO fire on neighbouring shapes are both keyed on something this shape
lacks:

- The field-to-field reject (`MainListener.h`, the `IsUniqueTempFieldRead` leg added on
  `fix/uniq-family`) requires the DESTINATION to be an owning `unique` slot. `PlainSlot.p` is a
  plain borrow, so there are not two owners and that reject correctly declines.
- The owning-temp-field reject (`FromOwningTempField`) requires
  `IsOwningValueType(rightNV.TypeAndValue.TypeName)` - true only when the POINTEE has a
  destructor. Give `Node` a `~Node() {}` and this exact program IS diagnosed
  ("taken from a temporary into a longer-lived location", covered by
  `Test/errors/err_unique_borrow_into_field.cb`). A dtor-less pointee falls through both.

So the discriminator that is missing is not ownership of the destination and not the pointee's
destructor - it is that the SOURCE is a field of a temporary that will be destructed at the end
of the statement, whoever the destination is.

## Fix direction

The provenance already exists: `rightNV.FromOwningTempField` is set for this read. The narrow
change is to reject a borrow-typed destination taking a field off an owning temp, gated on the
temp provenance rather than on `IsOwningValueType(TypeName)`. Watch the polarity - the same
provenance is set for perfectly legal reads that do not outlive the statement (e.g.
`int x = makeBox().t->v;`), so the reject must require that the value is STORED into a slot
outliving the statement, not merely that it was read off a temp.

The message should point at binding the whole call result to a local first, which is what the
sibling temp diagnostics already recommend.

## Test coverage

None. `Test/errors/err_unique_borrow_into_field.cb` covers only the destructor-bearing variant
(`tempPointerFieldIntoPlainSlot`), which reaches a different reject.

Related: [[interface-issue-queue]]
