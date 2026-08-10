# An implicit consume of a FIELD of a borrow local double-frees

Measured 2026-08-10 during the review of `fix/aliasres`. That commit rejects the EXPLICIT spelling
(`move k.item`, cell 3 of [[alias-borrow-remaining-launder-sites]]); the IMPLICIT consume of the
same field - a plain `=` or a call argument that the owning-assign classifier turns into a Move -
walks straight past the new guard. Same root cause, one keyword apart.

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
    { Wrap kw = o.getw(); Box other; other = kw.b; }   // implicit consume of a borrow's field
    printf("owner=%d\n", o.w.b.item->id);              // garbage
    return 0;
}
```

Measured on `fix/aliasres` and on its base `0535f48` alike (`scratch/rev_b_implicit_assign.cb`):
prints `other=5 dtor=0`, then `after dtor=1`, then a garbage `owner=`, exit 134. The call-argument
spelling `sinkBox(kw.b)` into a `move Box` parameter is identical (`scratch/rev_r1_call_arg_implicit.cb`,
exit 134 on both binaries). The DECLARATION spelling `Box taken = kw.b;` is benign - it binds as a
borrow, frees nothing, and reads live memory.

## Root cause

`kw` shallow-copies `o.w`, so `kw.b` and `o.w.b` hold the same `unique Res*`.
`ClassifyOwningAssignSource` sees a non-copyable owner and picks `AssignSourceKind::Move`, which
transfers the bits to the destination without either binding losing its destructor. The borrowed-by-
value-PARAMETER twin of exactly this is already guarded at both consume sites -
`RejectConsumeOfBorrowedByValueParamField` at `MainListener_Expressions.cpp:2184` (the `=` arm) and
at `MainListener_Declarations.cpp:4184` (the decl arm) - but it asks `RootIsBorrowedByValueParam`
only. The `alias`-borrow-local twin `RootIsAliasBorrowLocal` (added by `fix/aliasres`, set at the
field-access site in `MainListener_PostfixExpression.cpp:928`) is asked ONLY inside
`ParseMoveExpression`, so nothing that does not spell `move` reaches it. Note the decl arm's hoisted
`srcFieldPathNV` (`MainListener_Declarations.cpp:3269`) copies `RootIsBorrowedByValueParam` and not
its new twin.

## Fix direction

Ask the new fact at the two implicit-consume sites the parameter twin already guards, i.e. extend
`RejectConsumeOfBorrowedByValueParamField` (or add its sibling) to fire on
`RootIsAliasBorrowLocal`, and propagate the field into `srcFieldPathNV` at
`MainListener_Declarations.cpp:3269` so the decl arm can see it.

Accept set to build first: the same consume off a NON-borrow local (must still transfer), off a
borrow local that was RE-BOUND (it owns again - the `alias_rebind_then_move_field_*` legs), off a
by-reference lambda capture (its storage IS the outer owner's - the `refcap_field_move_*` legs in
`Test/test_function_ptr.cb`), and the benign DECLARATION spelling `Box taken = kw.b;`, which must
keep compiling and reading live memory.

## Severity

Silent double free (compile 0, garbage read, abort at teardown, no diagnostic). Pre-existing on the
base commit, so it is residue of the same family rather than a regression.
