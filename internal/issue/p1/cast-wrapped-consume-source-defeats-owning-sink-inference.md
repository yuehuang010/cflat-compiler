# A cast-wrapped consume source `(T)p` / `p as T` defeats owning-sink inference and double-frees

Filed 2026-08-09 by the review of `fix/parenmv`. That round peeled redundant PARENTHESES off a
consume source before intersecting with the parameter list (`BareSourceText()` in
`cflat/MainListener.h`). A same-type CAST wrapper is the other text wrapper the semantic consume
arms see straight through, and it was never enumerated: measured IDENTICAL on `42a8caa` and on
`fix/parenmv`, so it is not a regression of that fix - it is the same pre-existing hole under a
different spelling.

Severity: double free (abort, rc 134), plus a missing caller-side `use of moved variable`
rejection.

## Repro

```cflat
int dtor = 0;
struct Res { int id = 0; ~Res() { dtor = dtor + 1; } };
struct UBox { unique Res* item = nullptr; };
UBox umk(int n) { UBox b; b.item = new Res(); b.item->id = n; return b; }

void fcast(UBox p)  { UBox o = (UBox)p; }            // rc 134
void fas(UBox p)    { UBox o = p as UBox; }          // rc 134
void fbrace(UBox p) { UBox[2] d = { (UBox)p }; }     // rc 134
void fbare(UBox p)  { UBox o = p; }                  // rc 0 (control)

extern int main() { UBox a = umk(5); fcast(a); return 0; }
```

Measured (`scratch/rev_cast_paren.cb`, `rev_as_src.cb`, `rev_cast_brace.cb`, `rev_cast_reject.cb`):
every cast/`as` spelling frees once in the callee and then aborts on the caller's `a` - rc 134 on
BOTH binaries. The caller-side half is lost too: `fcast(a); a.item->id` reads garbage (rc 134)
where the bare-source control `fbare(a); a.item->id` is rejected with
`use of moved variable 'a'`.

`move (UBox)p` does not parse (`extraneous input 'p' expecting ';'`), so only the three storing
spellings are reachable.

## Root cause

Same site as the parenthesis case: `CollectConsumedStoreNames` /
`CollectUnconditionalMovedNames` (`cflat/MainListener.h`) record the source subtree's text and
`ApplyOwningSinkInference` intersects it with the parameter names. `(UBox)p` yields `"(UBox)p"`
and `p as UBox` yields `"pasUBox"`, neither of which equals `p`, so the parameter is never made a
sink while the callee consumes anyway. `BareSourceText()` deliberately peels only the
`primaryExpression : '(' expression ')'` alternative; a `castExpression : '(' typeName ')'
castExpression` node has more than one child and is left alone.

Confirmed by witness (`scratch/rev_cast_witness.cb`): adding an unrelated bare-name store
`if (c == 999) { UBox z = p; }` to the same function puts `p` in the collected set, and the cast
case immediately flips from rc 134 to a compile-time `use of moved variable 'p'`. The collector's
name set is the whole mechanism.

## Fix direction

Extend the source normalization to peel a REDUNDANT cast - one whose target type is the same type
as the operand - in the same place `BareSourceText()` peels parentheses, and the `as` form with
it. This needs its own accept-set first: a TYPE-CHANGING cast (derived-to-base, interface boxing,
a numeric conversion) is not a whole-value consume of the named variable and must keep failing the
intersection, and the collectors run in the scanner where the operand type may not be resolvable -
so a purely syntactic "peel any cast" rule is likely wrong and a type-aware peel may not be
available at that point. An alternative is to stop matching on text entirely and thread the
operand's resolved `NamedVariable` through instead (see the related issue below), which would
close this spelling and the semantic cells at once.

## Related

- `internal/issue/p1/parenthesized-operand-loses-named-variable-provenance.md` - the semantic half
  of the same family; its fix direction (thread `NamedVariable` provenance instead of comparing
  spellings) would subsume this one.
- `internal/fix-issue-lessons.md` - the `fix/parenmv` record (the syntactic half, parentheses
  only).

### New cell measured by `fix/retfield` (2026-08-10)

`return w.b as UBox;` out of a dying local (`scratch/rf_10_cast.cb`) and `return b.s as string;`
(`scratch/rf_39_strparen.cb`) are rc 133 / the empty string on both the merge base and
`fix/retfield`, whose new return-position consume and string-field deep-copy arms fix every bare
spelling of the same programs. Same mechanism as the parenthesized twin: the cast wrapper drops
the operand's `FieldName` / field-path provenance before the arm sees it.
