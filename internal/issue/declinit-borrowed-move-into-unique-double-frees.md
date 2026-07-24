# `unique R* b = move p;` from a BORROWED parameter double-frees (decl-init path only)

Filed 2026-07-24, surfaced while fixing `unique-pointer-reassign-via-move-loses-ownership.md`
(fixed in d33b9cf). Pre-existing on master before that commit and unchanged by it, but that
commit closed the same hole on the ASSIGNMENT path, so decl-init is now the odd path out.

## Summary

Moving a BORROWED pointer parameter into a `unique` local transfers nothing - the caller still
owns the object and frees it at its own scope exit - but the decl-init path adopts it as owned
anyway, so it is freed twice.

## Repro (verified against d33b9cf, exit 134)

```cflat
struct R { int v = 0; ~R() { printf("dtor %d\n", v); } };
void borrowIt(R* p)
{
    unique R* b = move p;              // p is borrowed; transfers nothing
    printf("in borrowIt b->v=%d\n", b->v);
}
extern int main()
{
    unique R* a = new R();
    a->v = 42;
    borrowIt(a);
    return 0;
}
```

Output:
```
in borrowIt b->v=42
dtor 42                 <- freed inside borrowIt, while main still owns it
dtor -375517198         <- main's scope exit, freeing already-freed memory
exit 134 (abort)
```

## The asymmetry this leaves

Three paths handle borrow-into-unique; only decl-init is still wrong:

| path | plain `b = p` | `b = move p` |
|------|---------------|--------------|
| assignment (`b = ...`) | rejected (d33b9cf) | rejected (d33b9cf) |
| struct field | rejected (`RejectBorrowIntoUniqueField`) | rejected |
| decl-init (`unique R* b = ...`) | rejected ("cannot initialize unique 'b' from a borrowed value") | **ACCEPTED - double frees** |

So decl-init already rejects the plain borrow; it is specifically the `move` spelling that slips
through the D5 check.

## Root cause

`ParseMoveExpression` (MainListener.h, around 14858) sets `compiler->lastOwningResult = true`
unconditionally for a thin-pointer move, recording the real provenance only in
`result.IsBorrowed` (around 14876). The decl-init `srcIsOwningMove` branch keys off the former
and never consults the latter. The assignment path was given a `rightNV.IsBorrowed` gate plus a
diagnostic in d33b9cf (MainListener.h:10353-10369); decl-init has no counterpart.

## Fix direction

Mirror d33b9cf: gate the decl-init `srcIsOwningMove` adoption on `!rightNV.IsBorrowed` and emit
the same shape of diagnostic, modelled on `RejectBorrowIntoUniqueField`. The assignment-path
message is a good template:

> cannot assign borrowed parameter 'p' to unique local 'b' - the caller still owns it and frees
> it on scope exit, so this would free it twice. Declare the parameter 'move p' to transfer
> ownership, or drop 'unique' from 'b' if it only borrows.

Note the message nit recorded against the assignment version: when the borrow arrives via a
plain field of a borrowed struct param (`b = s->raw;`) the message names `s` and suggests
`move s`, which would not fix that case. The "drop 'unique'" suggestion is the applicable one.
Worth fixing in both places at once.

Also unaddressed: the unique-INTERFACE leg of the assignment path still uses the older
`lastOwningResult`-based snapshot rather than the narrowed `srcIsOwnedPtrRhs`, so it likely has
the same exposure. Check it when fixing this.

Regression test: extend `Test/errors/err_move_borrowed_ptr_into_unique_field.cb`, which already
has the field leg and the assignment leg - add the decl-init leg beside them.
