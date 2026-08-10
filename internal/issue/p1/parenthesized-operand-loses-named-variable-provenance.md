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

Thread the operand's `NamedVariable` (at minimum `CallerName`, `Storage`, and the borrow flags)
out through the parenthesized-primary lowering, instead of peeling text at each consumer. A
text-level peel was the right fix for the two purely syntactic collectors, but it cannot help
here: these arms need the resolved variable, not its spelling.

## Related

- `internal/fix-issue-lessons.md` - the `fix/parenmv` record (the syntactic half).
- `internal/issue/p3/discard-position-not-threaded-through-parens-and-ternary.md` - same
  parenthesization family, cosmetic.
- `internal/issue/p1/owning-field-of-a-by-value-struct-parameter-double-frees-on-consume.md` -
  independent; `UBox o = w.b;` on a by-value `Wrap` parameter is rc 134 with AND without parens
  (`scratch/pm_c1`/`pm_c2`), so it is not a parenthesization bug.
</content>
</invoke>
