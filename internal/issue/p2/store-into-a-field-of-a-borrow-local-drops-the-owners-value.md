# A plain `=` into a FIELD of a borrow local drops the real owner's value

Measured 2026-08-10 by `fix/aliasres` while building the accept set for
[[alias-borrow-remaining-launder-sites]] cell 1. Same family, different question, so it was
deliberately NOT folded into that commit: cell 1 asks "is the DESTINATION binding a borrow", this
one asks "is the destination field's PATH ROOT a borrow", which is a field-store-side change with
its own accept set.

## Repro

```cflat
int dtorCount = 0;
struct Res { int id = 0; ~Res() { dtorCount = dtorCount + 1; } };
struct Box { unique Res* item = nullptr; };
struct Wrap { Box b; alias Box get() { return this.b; } };
Box makeBox(int id) { Box x; x.item = new Res(); x.item->id = id; return x; }
struct Outer { Wrap w; alias Wrap getw() { return this.w; } };

extern int main()
{
    Outer o; o.w.b = makeBox(5);
    { Wrap kw = o.getw(); kw.b = makeBox(2); }
    printf("owner=%d\n", o.w.b.item->id);   // garbage
    return 0;
}
```

Measured on `fix/aliasres` (identical on `0535f48`, the merge base): prints `in=1 id=2`, then
`owner=<garbage>`, exit 134. Unchanged by the cell 1 / cell 3 fix.

## Root cause

`kw` shallow-copies `o.w`, so `kw.b` and `o.w.b` are the same `unique Res*`. The struct-FIELD arm of
the drop-old in `ParseAssignmentExpression` (`MainListener_Expressions.cpp`, the
`destIsStructField || destIsLocalOwningVar || destIsDropOldElem` block) destructs the old field
value unconditionally, which frees the pointee `o.w` still holds. Cell 1's
`destIsAliasBorrowLocal` explicitly requires `!destIsStructField && namedVar.FieldName.empty()`, so
it does not fire here.

## Fix direction

The recorded root fact cell 3 added is already the right question: `RootIsAliasBorrowLocal`
(`LLVMBackend.h`, set at the field-access site in `MainListener_PostfixExpression.cpp`) is true for
`kw.b`. Either suppress the field drop-old when the destination's path root is a borrow (the cell 1
shape - but a field cannot carry a per-field retirement, so the new value would LEAK with no way to
retire it), or REJECT the store the way cell 3 rejects the `move` twin. The reject is likely correct
here: unlike a whole-local rebind there is no binding whose classification could retire.

Accept set to build first: the same store off a NON-borrow local (must keep its drop-old), and a
by-reference lambda capture root (its storage IS the outer owner's, so the drop-old is correct -
frozen as `refcap_rebind_*` in `Test/test_function_ptr.cb`).

## Severity

Silent double free / use-after-free (compile 0, garbage read, abort at teardown, no diagnostic).
Pre-existing on the merge base, so it is residue rather than a regression.

## Second cell, same question: a BY-VALUE STRUCT PARAMETER as the destination root (2026-08-10)

Measured on `fix/uniq-implicit-move` and IDENTICALLY on the merge base `c9405da`, so it predates
that change. Probe: `scratch/rev_destbyval_new.cb`.

```cflat
struct H { unique Node* slot = nullptr; };
void f(H h) { h.slot = new Node(); }   // drop-old frees the CALLER's pointee
extern int main()
{
    H a = default; a.slot = new Node(); a.slot->v = 1;
    f(a);                               // a.slot now dangles
    ...
}
```
```
freed=1 anull=0     compile rc 0, run rc 133 - on BOTH binaries
```

The callee's `h` is a shallow copy of the caller's struct, so the `unique`-field drop-old at the
store frees a pointee the caller still owns and still frees. Exactly this file's question - "is the
destination field's PATH ROOT a borrow" - with a by-value parameter as the root instead of an
`alias` local; `RootIsBorrowedByValueParam` is already recorded at the field-access site and is the
fact to gate on.

Note the asymmetry now pinned in the opposite direction: the SOURCE side of the same root IS
guarded (`RejectConsumeOfBorrowedByValueParamField`, extended to the implicit-move store path on
`fix/uniq-implicit-move`, legs in `Test/errors/err_unique_borrow_into_field.cb`). The DESTINATION
side is this cell and remains open.
