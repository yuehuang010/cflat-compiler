# A parenthesized operand loses its NamedVariable provenance, so ownership arms misfire

Filed 2026-08-09 by the `fix/parenmv` round, which closed the SYNTACTIC half of the
parenthesization problem (`CollectConsumedStoreNames` / `CollectUnconditionalMovedNames` now peel
redundant parens before matching parameter names). These three cells are the SEMANTIC half: they
are not driven by those collectors at all, and each was measured identical before and after that
fix.

Severity: double free (rc 134) and a null deref (rc 139).

## Repro

Shared preamble (`scratch/pm_common.txt`):

```cflat
int dtor = 0;
struct Res { int id = 0; ~Res() { dtor = dtor + 1; } };
struct UBox { unique Res* item = nullptr; };
UBox umk(int n) { UBox b; b.item = new Res(); b.item->id = n; return b; }
```

### (a) `return (p)` of a borrowed by-value owning parameter - `scratch/pm_c7b/pm_c8b`

```cflat
UBox f(UBox p) { return p;   }  // rc 0
UBox f(UBox p) { return (p); }  // rc 134
extern int main() { UBox a = umk(5); UBox r = f(a); return 0; }
```

The IR diff of `main` is the whole story: with `return p` the caller emits NO `%r` alloca and no
`UBox.dtorfull` for the result - the result is recognized as still belonging to `a`. With
`return (p)` the caller allocates `%r`, stores the returned bits, and destructs it, so `r` and `a`
free the same `Res`.

### (b) A CONDITIONAL `move (p)` of a COPYABLE owning parameter - `scratch/pm_d3/pm_d4`

```cflat
struct CBox { unique Res* item = nullptr; CBox copy() { ... } };
void f(CBox p, int c) { CBox o; if (c != 0) { o = move p;   } }  // rc 0,   caller's src == 5
void f(CBox p, int c) { CBox o; if (c != 0) { o = move (p); } }  // rc 134, caller's src garbage
```

Both spellings now put `p` in `consumedNames`, and the concrete copyability gate correctly keeps
the parameter a borrow in both. The divergence is inside the callee: `move (p)` moves out of the
caller's storage, `move p` copies.

### (c) A parenthesized store source is not marked moved-from on a LOCAL - `scratch/pm_j3/pm_j4`

```cflat
extern int main() { UBox x = umk(5); UBox o = x;   printf("%d", x.item->id); }  // rejected:
                                                          // "use of moved variable 'x'"
extern int main() { UBox x = umk(5); UBox o = (x); printf("%d", x.item->id); }  // rc 139
```

Locals are never touched by `ApplyOwningSinkInference` (it intersects with the parameter list), so
this is a third, independent site.

## Root cause (direction, not yet pinned to a line)

All three lose the operand's `NamedVariable` provenance - `CallerName` and the borrow/ownership
origin it carries - when the operand is wrapped in the `primaryExpression : '(' expression ')'`
alternative. `ParsePrimaryExpression` returns a raw `llvm::Value*` for that alternative, and the
semantic ownership arms downstream (`IsBorrowedStructParameter(compiler, returnNV.CallerName)`,
the move-of-a-borrow decision, the moved-from marking of a named store source) all key on a name
that is now empty.

## Fix direction

Thread the operand's `NamedVariable` (at minimum `CallerName`, `Storage`, the borrow flags, and
`IsViewElement`) out through the parenthesized-primary lowering, instead of peeling text at each
consumer. A
text-level peel was the right fix for the two purely syntactic collectors, but it cannot help
here: these arms need the resolved variable, not its spelling.

### (d) A parenthesized `T[]` VIEW element loses `IsViewElement` (measured 2026-08-10)

`fix/viewelem` added `NamedVariable::IsViewElement`, so the flag list above now has a fourth member
and a fourth cell. The fixed-array oracle is CORRECT in both spellings; only the view spelling
diverges under parens:

```cflat
Box[2] base; base[0] = umk(1); Box q = (base[0]);              // rc 0, source nulled - MOVES
Box[] v = base;               Box q = (v[0]);                  // rc 133, source NOT nulled
Box[] v = base;               (v[0]) = a;                      // old element orphaned (leak)
```

(The `string` element read is rc 133 under parens in BOTH array spellings, so that one is a plain
instance of this issue rather than a view/fixed asymmetry.)

## Related

- `internal/fix-issue-lessons.md` - the `fix/parenmv` record (the syntactic half).
- `internal/issue/p3/discard-position-not-threaded-through-parens-and-ternary.md` - same
  parenthesization family, cosmetic.
- The by-value-parameter field consume (that issue file was deleted by `fix/bvfield`, which
  rejected it). It was correctly filed as independent - `UBox o = w.b;` on a by-value `Wrap`
  parameter was rc 133 with AND without parens - but the two are now COUPLED in the other
  direction: after `fix/bvfield` the bare spelling is a hard error and the parenthesized one
  (`UBox o = (w.b);`, `UBox o = move (w.b);` - `scratch/bv_13`/`bv_o2`) still compiles and still
  double-frees, because the guard reads `OwningStructName` / `FieldName` / `FieldPathRoot` and the
  parenthesized primary carries none of them. This is the concrete hole a fix here closes, and it
  is a cell of THIS issue, not of that one.

### New cell measured by `fix/retfield` (2026-08-10)

The `return` position joined this family. `fix/retfield` gave the return path the same consume
decision the store arms take for an owning FIELD path, so `UBox mk() { Wrap w; w.b = umk(3);
return w.b; }` went rc 133 -> rc 0.

Two of the three wrapper cells measured against that arm survive, and the third only looks closed:

- `return b.s as string` / `return w.b as UBox` (`scratch/rf_10_cast.cb`,
  `scratch/rf_39_strparen.cb`): unchanged, rc 133 / the empty string on both binaries. The cast
  drops the operand's `Storage` as well as its `FieldName`, so no shape test can reach it - this
  cell belongs to the cast sibling issue.
- `return (b.s);` (`scratch/rf_27_paren_string.cb`): still the empty string (`p=0`). The `string`
  arm keys on `FieldName`, which the parenthesized primary drops.
- `return (w.b);` (`scratch/rf_09_paren.cb`) is now rc 0 - **but not because this issue was
  fixed.** The owning-struct arm admits by GEP SHAPE (a two-index struct/array access), which the
  paren happens to preserve, so it consumes without ever seeing the field's provenance. The
  provenance it does NOT see is `RootIsBorrowedByValueParam`, so the borrowed-by-value-parameter
  spelling `UBox f(Wrap w) { return (w.b); }` is an UNDIAGNOSED double free (rc 133 before and
  after, `scratch/rf_26_paren_bvparam.cb`), while the bare `return w.b;` beside it is a hard
  error. Severity is unchanged by the widening - the bit-copying predecessor double-freed too -
  but the shape now silently reaches a consume arm, which is a worse place to be missing the
  guard. Closing this issue is what makes that spelling diagnosable.
