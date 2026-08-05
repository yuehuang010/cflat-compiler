# A `?:` / `??` join erases the code-value evidence, defeating the ARGUMENT gate as well as the store gate

Filed 2026-08-04 by `fix/codeval-store`, which closed the store paths of
`code-value-into-data-pointer-outside-overload-resolution` and measured this as the one axis its
destination-side reader structurally cannot see.

Severity: **P1 - memory-unsafe, silent.** A code address reaches a data pointer and is written
through, exit 138, no diagnostic.

## Repros - all exit 138 on `6e9ab46`, on `a846e6e` (the merge base) AND on `fix/codeval-store`

Store leg, `?:` (`scratch/cvs_d_ternary.cb`):

```cflat
import "function.cb";
struct Rec { int a = default; int b = default; };
double ro(double x) { return x + 1000.0; }
extern int main(int argc, char** argv)
{
    function<double(double)> w = ro;
    Rec* n = nullptr;
    Rec* r = argc > 0 ? w : n;   // exit 138 on both binaries
    r->a = 11;
    printf("a=%d\n", r->a);
    return 0;
}
```

Store leg, `??` (`scratch/cvs_d_nullcoal.cb`): `Rec* r = n ?? w;` - same result.

Brace-init leg (`scratch/cvs_r2_join_brace.cb`), measured rather than assumed after
`fix/codeval-store` added the aggregate sites: `Holder h = { p = argc > 0 ? w : n, n = 3 };` is
exit 138 on `6e9ab46`, on `a846e6e` and on `fix/codeval-store`. So the erasure defeats the
SYNTAX-axis sites (brace field init, array aggregate, field default, parameter default) exactly as
it defeats the declarator, assignment and return sites - the fix must serve all NINE store gate
sites plus the argument gate, which is the argument for doing it once at the source rather than
per site.

Every leg above was re-measured against `a846e6e` after `fix/codeval-store` was rebased onto it,
per spelling rather than inferred from a sibling: a baseline claim is a measurement, and the base
moved twice while this was in review.

**ARGUMENT leg (`scratch/cvs_d_ternary_arg.cb`) - this is the important one**, because it shows the
gap is NOT specific to the store paths:

```cflat
int lam(Rec* p) { p->a = 11; return p->a; }
...
printf("a=%d\n", lam(argc > 0 ? w : n));   // exit 138 on both binaries
```

`lam(w)` with the bare spelling is diagnosed by the gate `fix/funcptr-rebind` landed in
`ComputeOverloadFunction`. Wrapping the same value in a join makes it bind again.

## Root cause

Both gates are predicate reads over a `NamedVariable` / `TypeAndValue`
(`ArgumentIsCodeValue` -> `ArgumentIsFunctionPointerish`). A join produces a bare `llvm::Value`:
the `?:` spelling a PHI over two arms, the `??` spelling a load out of a slot. Neither carries a
TypeName, a recorded signature, or an `llvm::Function` `Primary`, and under opaque pointers the
arm value is an indistinguishable `ptr`. The codebase already knows this shape - the interface
upcast has to recover each arm from a ledger for exactly this reason
(`RegisterNullCoalesceJoin` / `UpcastTernaryPhiToInterface`, `MainListener.h`).

So this is NOT the same defect as the one `fix/codeval-store` closed. There the destination-side
reader was missing; here every reader is present and correct and the SOURCE evidence has been
erased before any of them run. That is why the fix was scoped out rather than folded in: it is a
recording change on the source side, and it must serve all three gates (argument, store, return)
or the two halves drift.

## Fix direction

Record-then-resolve, mirroring `fatInterfaceValueTypeNames_`: ledger a value as code by VALUE
IDENTITY where the facts are local (`LoadNamedVariable`, which already ledgers the fat-interface
name three lines away), then have `ArgumentIsCodeValue` consult the ledger - directly, and through
a join by walking a PHI's incoming values and a `??` join's recorded arms.

Recording cannot reject, so a missed site degrades to "no diagnostic" rather than to a false
rejection.

The ledger's lifetime must copy an existing one exactly, or it leaks across files: it needs a
`SaveBuilderState` / `RestoreBuilderState` pair and a `ResetForReanalysis` clear. Park it with
`nullCoalesceJoins_` / `interfaceBoxRecords_` (which survive `FlushOwnedTemps`), not with
`ownedNewTemps_` - the destination gate runs before the end-of-expression flush, but the argument
gate does not necessarily.

Accept set to re-measure before landing anything (all pass today, all in
`Test/test_function_ptr.cb::testCodeValueStoreAccepts`): an explicit `(Rec*)` / `(void*)` cast,
`function<T>*`, `function<T>[N]` and `function<T>[]`, a funcptr CALL RESULT whose return type is a
data pointer, and a join whose arms are both plain data pointers.
