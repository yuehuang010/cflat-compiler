# A cast on one join ARM launders its sibling arm (memory-unsafe accept)

Filed 2026-08-08 by review round 1 of `fix/cast-launder-occurrence`. The residual that fix
left deliberately, promoted from a review finding to its own file because it is a
memory-unsafe accept with a one-line repro.

**Severity: silent memory-unsafe accept, exit 138.** Defensible at P2 under the
residue-not-regression precedent used for the fix's own predecessor issue (see
`internal/issue/interface-issue-queue.md`): this spelling was ALSO accepted before
`fix/cast-launder-occurrence` (occurrence keying operates at argument granularity, one level
above the join itself, so it was never in scope to close this shape), so it is what that fix
could not reach, not a regression. Re-rank to P1 if the maintainer rules the
memory-unsafe-accept rubric wins.

## Repro (`scratch/rev/rev_p02_arm_launders_arm.cb`, `scratch/rev/rev_p05_arm_swapped.cb`)

```cflat
import "function.cb";
struct Rec { int a = default; int b = default; };
double ro(double x) { return x + 1000.0; }
int one(Rec* p) { p->a = 7; return p->a; }
extern int main(int argc, char** argv)
{
    printf("%d\n", one(argc > 100 ? (Rec*)ro : ro));  // compiles clean -> exit 138
    return 0;
}
```

Order-independent: `one(argc > 0 ? ro : (Rec*)ro)` (`rev_p05_arm_swapped.cb`) reproduces
identically. Both were measured compiling clean and exiting 138 on the pre-fix binary
(`f1b8116`) AND on the post-fix binary from this round - not a regression introduced by
`fix/cast-launder-occurrence`, and not closed by it either.

## Root cause

Occurrence keying (this fix's mechanism) scopes a cast's launder to the argument/field slot
that CONTAINS the join, via `BeginCastOccurrence`/`EndCastOccurrence` around each
independently-gated call argument, ctor argument, brace-init field, and XML attribute. Both
arms of a single `?:`/`??` join are evaluated inside the SAME slot and therefore share the
SAME occurrence id - the cast on one arm (`(Rec*)ro`) and the bare mention on the sibling arm
(`ro`) both register/query under identical (value, occurrence) keys, so the ledger cannot tell
them apart. Argument-granularity keying is structurally the wrong level to separate two arms
of one join; only a finer key (per-arm, not per-argument) could close this.

## Fix direction

Not investigated. A per-arm occurrence (bump around each `?:`/`??` arm's own evaluation, not
just around the enclosing argument) is the natural next refinement, but interacts with the
existing arm-recursion in `JoinArmCarriesCodeValue`/`JoinCarriesCodeValue` and needs its own
accept-set sweep (an arm that legitimately reads the SAME cast value twice, e.g. `c > 0 ?
(Rec*)ro : (Rec*)ro`, must stay accepted).

Related: [[same-statement-cast-launders-join-code-evidence]] (closed by this fix, the
predecessor of this residual), [[interface-issue-queue]]
