# `(*p)++` is a silent no-op - the increment is written to a temp and lost

Filed 2026-08-10 by the review of `fix/wrapprov` (report-only finding 6), then measured
independently. PRE-EXISTING and unchanged by that commit: identical on merge base `6a8e7a9` and on
`fix/wrapprov`. The neighbouring STORE bug (`(*p) = 9`, also silent) WAS fixed there, which is what
makes this leftover worth a file - the two read as one behaviour and now differ.

Severity: **P2**. Silent DATA LOSS with no diagnostic. Not P1 only because it corrupts no
ownership state and frees nothing twice - the write is simply dropped.

## Repro

`scratch/wp_inc_axes.cb`, Release, macOS arm64, warm `--init-local`:

```cflat
struct S { int n = 0; };
extern int main() {
  int x = 5; int* p = &x;      (*p)++;        printf("deref_post=%d\n", x);   // 5  - WRONG, want 6
  int y = 5;                   (y)++;         printf("name_post=%d\n", y);    // 6  - correct
  int[2] a; a[0] = 5;          (a[0])++;      printf("elem_post=%d\n", a[0]); // 6  - correct
  S s; s.n = 5; S* sp = &s;    (*sp).n++;     printf("field_post=%d\n", s.n); // 6  - correct
  S t; t.n = 5;                (t.n)++;       printf("dotfield_post=%d\n", t.n); // 6 - correct
  int z = 5; int* q = &z;      ((*q))++;      printf("dbl_post=%d\n", z);     // 5  - WRONG
  return 0; }
```

Measured `deref_post=5 name_post=6 elem_post=6 field_post=6 dotfield_post=6 dbl_post=5` on BOTH
binaries. `(*p)--` is the same (`scratch/wp_inc_paren.cb`: `a=5 b=5`). The bare twin `*p = *p + 1`
is correct (`e=6`), and so is the compound form `(*q) += 1`, which `fix/wrapprov` repaired from
`d=5` to `d=6` - so the parenthesized DEREF is now the only spelling in the family still wrong,
and only under `++`/`--`.

`++(*p)` does not parse at all (`mismatched input '++' expecting '}'`) and is a separate gap.

## Root cause (direction, not pinned to a line)

`MainListener::ParsePostfixExpressionInner` restores a parenthesized primary's `Storage` (and, since
`fix/wrapprov`, its `BaseType`) so the STORE arms write through. The `++`/`--` suffix is handled
inside the same postfix walk, on `namedVar` as it stands when the token is reached - and for the
parenthesized-deref shape it reads the loaded value rather than the restored address, so the
incremented result is stored back into a temporary. `ProcessPlusPlus()` is called inside
`ParsePrimaryExpression`'s paren alternative *before* the outer walk restores anything, which is the
first place to look.

The comment at `cflat/MainListener_PostfixExpression.cpp` (the `namedVar.Storage =
lastParenExprStorage;` site) used to CLAIM this case worked; `fix/wrapprov` corrected the comment to
point here rather than loosen the claim.

## Fix direction

Make the `++`/`--` suffix take the same restored `Storage`/`BaseType` pair the store arms take, and
prove it with the DOUBLE-paren spelling too (`((*q))++`) - the restore has to be a loop, not one
unwrap. Accept-set to hold: `(y)++`, `(a[0])++`, `(*sp).n++`, `(t.n)++` are correct today and are
pinned by `plv_*` in `Test/test_basic.cb`; add the `++` twins beside them.
