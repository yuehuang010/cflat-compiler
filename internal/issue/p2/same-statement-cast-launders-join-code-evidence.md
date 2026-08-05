# A same-statement data cast launders a join's code evidence (memory-unsafe accept)

Filed 2026-08-05 by round 2 of the `fix/joinledger` review. The residual that fix left
deliberately, promoted from a landed-record footnote to its own file because it is a
memory-unsafe accept with a one-line repro.

**Severity: silent memory-unsafe accept, exit 138.** Defensible at P2 only under the
residue-not-regression precedent ([[unique-field-to-field-interface-receiver-residues]]): the
spelling was also accepted before `fix/joinledger` (the whole join axis was), so this is what
that fix could not reach, not a regression. Re-rank to P1 if the maintainer rules the
memory-unsafe-accept rubric wins.

## Repro (round-2 probe `scratch/rev2/r2/g20_residual_twoarg.cb` on the fix branch)

```cflat
import "function.cb";
struct Rec { int a = default; int b = default; };
double ro(double x) { return x + 1000.0; }
int two(void* v, Rec* p) { p->a = 7; return p->a; }
extern int main(int argc, char** argv)
{
    Rec* n = nullptr;
    printf("%d\n", two((void*)ro, argc > 0 ? ro : n));  // compiles clean -> exit 138
    return 0;
}
```

The control - same program with the cast hoisted to its own statement
(`void* v = (void*)ro;` on the previous line) - IS diagnosed. An ordinary two-argument call,
not a contrived self-referential expression.

## Root cause

`codeValueDataCasts_` (the launder that keeps the advised escape hatch `(Rec*)w` compiling) is
keyed on `llvm::Value*` alone and retired at the statement boundary (`FlushOwnedTemps`). A
NAMED FUNCTION is one module-level `llvm::Function` constant, so within a single statement the
cast at argument 0 and the bare mention at argument 1 are the SAME Value: the cast launders
every mention of that function for the rest of the statement. Cross-statement and
cross-function leaks are already closed (statement scoping, per-function clear); only the
same-statement window remains.

## Fix direction

Occurrence keying: key the launder entry on (value, syntactic cast site) - a parse-context
pointer or a monotonic cast id - so `(void*)ro` launders only the expression the cast wraps,
not other mentions of `ro` in the same statement. The accept cells that must keep compiling
are pinned already: `c ? (Rec*)ro : n` and `c ? (Rec*)(ro) : n`
(`Test/test_function_ptr.cb::testCodeValueJoinAccepts`), plus the bare `(Rec*)w` escape hatch
in every position. Reordering `isa<Function>` ahead of the launder check is NOT the fix - it
was mutation-tested during review and false-rejects both pinned cast cells.

Related: [[join-defeats-the-closure-widen-gate]], [[interface-issue-queue]]
