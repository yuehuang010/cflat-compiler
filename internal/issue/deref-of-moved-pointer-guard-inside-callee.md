# False positive: moved-pointer deref guarded only by a conditionally-terminating callee

## Summary

The cross-block deref-of-moved-unique-pointer diagnostic (`nulldf`, `cflat/MoveDataflow.h`) is
intraprocedural. A guard that prevents the dereference by calling a function that CONDITIONALLY
terminates the process is invisible to it, so the deref is reported even though it can never
execute after the move.

## Repro

```cflat
struct R { int v = 7; };
void dieIf(bool c) { if (c) { printf("fatal\n"); exit(1); } }
int f(int n)
{
    unique R* a = new R();
    bool moved = false;
    if (n > 100) { unique R* b = move a; moved = true; }
    dieIf(moved);        // terminates exactly when the move happened
    return a->v;         // reported: "dereference of moved variable 'a'"; safe
}
```

`f(1)` returns 7; `f(150)` prints "fatal" and exits 1. The null deref never happens. Reported
whether `dieIf` is defined before or after `f`.

## Root cause

The analysis reports a deref `D` witnessed by move `M` iff `CD*(D)` is a subset of `CD*(M)` and
`x` is not read / revived / escaped on the `M -> D` path. A terminating guard suppresses via the
noreturn handling in `ComputePostDominators` (`BlockTerminatesProgram`) only when the terminating
call is visible in the CALLER's CFG. Here the branch-and-exit live inside `dieIf`'s body: the
caller's CFG is a straight line through the call (`dieIf(moved);` and the deref share one basic
block), `CD*(D)` is empty, and `moved` is not `a`, so neither condition fires.

`dieIf` is NOT noreturn - it returns when `c` is false - so no noreturn inference, at any pass or
in any declaration order, can close this. Closing it would require interprocedural
conditional-termination modeling (this call may-not-return, correlated with an argument correlated
with the move) PLUS splitting caller blocks at such call sites, since block-granular control
dependence cannot otherwise separate `dieIf(moved);` from the deref that follows it in the same
block. That is out of proportion, and correlated-flag reasoning is the exact family that produced
false positives in the three earlier designs of this diagnostic.

## Boundary statement

The diagnostic's guarantee is: **any guard VISIBLE IN THE CALLER'S CFG suppresses.** Skipping
guards suppress via control dependence, whatever they are written in terms of - a bool, an int, a
struct field, a global, a two-hop chain, a helper call, a correlated scrutinee, or an expression
the compiler cannot interpret at all; none are recognized and none need to be. Terminating guards
suppress via the noreturn handling, covering `exit` / `abort` / `_Exit` / `quick_exit`, any callee
carrying the LLVM `noreturn` attribute (e.g. from C interop), and any function this compiler has
itself PROVEN never returns (see below). A guard implemented inside a callee's body that only
sometimes terminates, OR THAT THIS COMPILER CANNOT PROVE NEVER RETURNS - an indirect or lambda
call, where the callee is not statically known, and recursion, self or mutual - is outside that
guarantee. This is the standard intraprocedural boundary - the same one clang's analyzer has
without CTU - not a hole in the argument. Real-world severity is low: infinite recursion and
noreturn lambdas used deliberately as guards are rare.

Recorded so it is not re-litigated: an earlier revision of this file claimed the read-kill closed
the terminating-guard case, on the premise that "forming that guard reads `x`". That premise is
FALSE and was disproved by repro. A terminating guard need not mention `x` at all
(`if (moved) { exit(1); }`), which is why the noreturn handling in `ComputePostDominators` exists.

## What IS closed: the unconditionally-terminating wrapper

A wrapper that never returns on any path is provable and IS handled:

```cflat
void die() { printf("fatal\n"); exit(1); }   // truly noreturn, just carries no attribute
...
if (moved) { die(); }
return a->v;                                  // NOT reported
```

As each function body completes, `LLVMBackend::RunNullDerefDataflow` runs
`nulldf::FunctionNeverReturns` - a walk from entry that does not explore past a block containing a
terminating call, asking whether any live `ret` remains - and records the result in the backend
side table `provenNoReturn_`, which `BlockTerminatesProgram` consults. A side table rather than
the LLVM `noreturn` attribute deliberately: the attribute would also license optimizations, a far
wider blast radius than this diagnostic needs.

This is order-dependent, and never in the FALSE-POSITIVE direction: proving a wrapper noreturn can
only suppress a report or sharpen control dependence into a correct one. A wrapper defined AFTER
its caller is not yet proven when the caller is analyzed, so the caller's report stands (verified
both ways).

It is NOT monotone in the "only removes reports" sense, and a future reader should not file that as
a bug. Marking a block terminal SHRINKS post-dominator sets, which shrinks `CD*(D)` but can also
GROW `CD*(M)` - and a larger `CD*(M)` makes the subset test EASIER, so a report can APPEAR:

```cflat
void die() { printf("fatal\n"); exit(1); }   // defined here: reported. Defined after f: silent.
int f(int n) {
    unique R* a = new R();
    int s = 0;
    for (int i = 0; i < 3; i++) {
        if (n > 100) { die(); }
        else { s = s + a->v; }     // reported once die() is proven
        unique R* b = move a;
    }
    return s;
}
```

Proving `die` makes the then-arm terminal, so the branch stops post-dominating into the merge and
the edge `(branch -> else)` ENTERS `CD*(M)` - which is correct, because the move genuinely only
runs when the else edge was taken. The program segfaults, so the added report is a TRUE positive
produced by control dependence that was previously under-approximated. The by-construction
argument is unaffected: a passing subset test still means "D is under no decision M was not
under". Proving more never overrides a real guard - the null-guarded variant of the program above
stays silent in BOTH declaration orders.

## Workarounds (both verified to suppress)

- Null-guard the deref: `if (a != nullptr) { return a->v; }`. The deref becomes control-dependent
  on a branch the move was not under, and the guard reads the pointer. This is the idiomatic fix.
- Inline the check in the caller: `if (moved) { printf("fatal\n"); exit(1); }`.

## Accepted false negatives

Carried over from the now-closed cross-block issue. All cost a diagnostic; none can invent one.

- **A guard the analysis cannot connect to the move.** Any deref under a branch the move was not
  under is suppressed, even when that branch does not actually protect anything
  (`if (other != nullptr) { return a->v; }` after a conditional move of `a`). This is the price of
  the by-construction property: that program is structurally indistinguishable from the
  correlated-scrutinee false positive, so catching it would reopen that whole family.
- **Re-tested conditions.** `if (c) { move a; } if (c) { a->v; }` - two distinct branch edges, so
  no containment. Conditions are compared by edge identity, never by meaning; proving two
  conditions equivalent is deliberately not attempted.
- **Any non-dereference read between the move and the deref** suppresses, guard or not.
  `if (c) { move a; } if (a != nullptr) { } a->v;` is not reported.
- **A CONDITIONAL deref after an UNCONDITIONAL move.** An unconditional move has `CD*(M) = {}`, so
  containment holds only for a deref that is itself unconditional:
  ```cflat
  unique R* b = move a;          // CD*(M) = {}
  if (n > 0) { return a->v; }    // NOT reported, though 'a' is DEFINITELY null here
  ```
  The containment test is strictest exactly where the bug is most certain. Not an oversight to
  fix: `if (n > 0)` is indistinguishable from a guard, and containment is what buys the
  false-positive-freedom above.
- A deref inside a branch/loop condition, a `?:` arm, or a short-circuit `&&`/`||` right-hand side
  is suppressed by the same two conditions rather than by any special case.
- The `&`-escape latch (`AddressEscaped`) remains a permanent, function-wide "never diagnose this
  name again" bit. Intentionally conservative and NOT a defect - do not "fix" it into something
  that produces false positives.

## Separate residual: `unique <interface>` locals are still same-block only

The interface member/method dispatch site records no dereference event, so a conditional or
loop-carried move of a `unique <interface>` local followed by a dispatch is not diagnosed:

```cflat
unique IBox ig = new BoxImpl();
if (RuntimeFalse()) { unique IBox ig2 = move ig; }
int t = ig.tag();          // NOT diagnosed; segfaults if the branch is ever taken
```

Deliberate: `Test/test_move.cb`'s `cross_block_conditional_move_then_deref_interface` asserts that
exactly this program compiles clean and runs. The shape is structurally identical to the
thin-pointer form, so there is no sound way to diagnose one and not the other - closing this needs
a decision on that test's intent first. The mechanical change is one call to
`RecordNullDerefFor(interfaceVar, ...)` at the interface dispatch site in `MainListener.h`;
`nulldf` already handles the name uniformly.

## Not a residual: dynamic reachability

`if (RuntimeFalse()) { move a; } a->v;` is reported even though the branch is never taken. By
design, and the acceptance case the maintainer specified: the analysis models the presence of a
guard on the dereference, not the reachability of the moving path.
