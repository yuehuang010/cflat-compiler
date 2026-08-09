# A fixed-array BRACE initializer from owning named sources double-frees

Filed 2026-08-09 by `fix/arrslot` while building the fixed-array-element destination matrix.
This is the DECL-INIT twin of the assignment gap that fix closed, and it is a different code
path (the array brace-list builder in `MainListener_Declarations.cpp`, not the assignment
expression path), so it was left out.

Severity: double free (abort).

## Repro

```cflat
int dtor = 0;
struct Res { int id = 0; ~Res() { dtor = dtor + 1; } };
struct Box { unique Res* item = nullptr; };
Box makeBox(int n) { Box b; b.item = new Res(); b.item->id = n; return b; }

extern int main()
{
    { Box a = makeBox(5); Box b = makeBox(6);
      Box[2] dst = { a, b };
      printf("ids=%d,%d\n", dst[0].item->id, dst[1].item->id); }
    return 0;
}
```

-> compiles 0, prints `ids=5,6`, then **rc 133**. Measured identical on `7beb979` and on
`fix/arrslot`.

## Root cause (hypothesis - verify)

The brace-list element stores are emitted by the declarator's array builder, which bit-copies
each element expression into the slot. It never asks the owning-store question at all, so a
named owning source is aliased rather than consumed: both `a` and `dst[0]` destruct the same
`Res`. The assignment spelling of the same store (`dst[0] = a;`) now defers to T - COPY a
copyable owner, MOVE a non-copyable one - via the fixed-array-element arm in
`MainListener_Expressions.cpp`.

Note the related-but-different `Test/errors/err_fixed_array_copy_owning_elem.cb`: a WHOLE-array
copy (`Owner[2] b = a;`) is already rejected. The brace form slips past that reject because it is
built element by element.

## Fix direction

Route each brace element store through the same decision the element assignment now uses
(`ClassifyOwningAssignSource`): copyable owner copies, non-copyable owner moves and zeroes the
source lvalue, with `MarkVariableMoved` for a named slot. No drop-old is needed here - the slots
are being constructed, not overwritten - which makes this the container-slot decision, not the
assignment one. `string` elements must keep their own machinery, as they do in the assignment
arm.
