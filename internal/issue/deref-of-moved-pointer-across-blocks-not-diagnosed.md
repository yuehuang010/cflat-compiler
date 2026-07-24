# Dereference-of-explicitly-moved-pointer guard is same-block only

Opened while fixing deref-of-explicitly-moved-pointer-unguarded.md (now fixed for the
straight-line case). This file tracks the deliberately narrow scope of that fix and the
crash shapes it does not cover.

## Summary

The fix added a compile-time check that rejects a DEREFERENCE (`->`/`.`/`*`/`[]`) of a
thin `unique` pointer (or `unique <interface>`) local that was explicitly `move`d out in
THE SAME LLVM basic block. This is sound and false-positive-free for straight-line code
(the filed repro's exact shape), but it is not a general solution: a move and a
dereference in DIFFERENT blocks - a conditional move, a loop-carried move, or any other
control-flow split between the move and the deref - is not diagnosed. The pointer is
still null and the deref still segfaults at runtime; there is simply no compile-time
error for it.

## Why same-block, not general

An earlier attempt used a flat per-scope `IsMoved`-style flag, OR-merged across if/else
branches and fed through the whole-module `RunMoveDataflow` fixpoint for loop-carried
cases (the same machinery the "use of moved variable" implicit-move diagnostic already
uses). That caught the loop-carried and if/else-divergent cases, but it rejected
legitimate, idiomatic defensive code that no reasonable compiler should flag:

```cflat
unique R* a = new R();
unique R* b = move a;
if (a != nullptr) { use(a->v); }     // FALSE POSITIVE: a is provably non-null here
```

```cflat
void fill(R** slot) { *slot = new R(); }
unique R* a = new R();
unique R* b = move a;
fill(&a);              // legal: caller escaped the address, may have rewritten it
return a->v;            // FALSE POSITIVE with the OR-merge approach
```

CFlat has no flow-sensitive null-narrowing anywhere in the language today (`IsNullable`
only drives default-init; `?.`/`??` lower to runtime branches with no compile-time
typestate). Building "clear the flag on a dominating `x != nullptr`" would mean building
the compiler's first null-narrowing pass as part of what was scoped as a crash fix - out
of scope. The same-block predicate was chosen instead because it is FP-free BY
CONSTRUCTION: an `if`, an `if`/`else`, and a loop body each start a new LLVM basic block,
so the guard structurally cannot fire across one - no enumerated carve-outs, no
narrowing logic, no risk of a new false positive as the language grows.

This applies equally to `unique <interface>` locals: the interface member-access dispatch
site is gated on the same `IsExplicitlyMovedNullHere` predicate as the thin-pointer
sites (not on the flat, OR-merged `IsMoved` flag, which an early draft of this fix used
there and which reintroduced exactly the false positive above for a conditional move of
an interface local). `unique <interface>` reads still have no post-move null-check escape
hatch (a plain `a != nullptr` after ANY move, same-block or not, is pre-existing-rejected
via `IsMoved` - see err_move.cb's whole-variable-move legs), so the only way to exercise
the interface FP-free-ness is an unguarded deref after a conditional move that is not
taken at runtime (see Test/test_move.cb's `cross_block_conditional_move_then_deref_interface`).

One more caveat, also FP-free by construction rather than by narrowing: `?:` (see the
ternary note below) evaluates both arms in the SAME block via `CreateSelect`, so a same-
block move immediately followed by a ternary deref (`a == nullptr ? 0 : a->v`) would
otherwise false-positive under the plain same-block rule. The guard is suppressed while
lowering either arm of a `?:` (a depth counter around `ParseConditionalExpression`'s arm
lowering, checked in `IsExplicitlyMovedNullHere`), so this compiles and runs correctly.

## Repros that still compile clean and crash at runtime

```cflat
struct R { int v = 0; };
extern int main(int argc, char** argv) {
    unique R* a = new R();
    if (argc > 100) { unique R* b = move a; }
    printf("%d\n", a->v);          // may deref a null a - no compile error, segfaults if taken
    return 0;
}
```

```cflat
struct R { int v = 0; };
extern int main() {
    unique R* a = new R();  a->v = 5;
    unique R* b = nullptr;
    for (int i = 0; i < 2; i++) {
        printf("iter %d v=%d\n", i, a->v);   // iteration 1 dereferences the just-moved a
        if (b == nullptr) { b = move a; }
    }
    return 0;
}
```

Both `--check` with 0 failures; both segfault (exit 139) under `--run`.

The `&`-escape latch (`AddressEscaped`) is also a coarse, permanent "never guard this
variable again" bit, not scoped to the escaped block - so a later, otherwise-diagnosable
same-block move+deref on a variable whose address was EVER taken earlier in the function
is silently un-diagnosed too. This is intentionally conservative (favors false negatives
over false positives) and not itself considered a defect, but is recorded here since it
compounds this issue's under-diagnosis.

## Adjacent, unrelated pre-existing bugs found while testing this fix

None of these are caused by or fixed by the deref guard; each was tripped over directly
while writing regression coverage for it and is filed as its own issue (one file per
issue, per repo convention) rather than bundled here:

- internal/issue/ternary-arms-evaluated-eagerly-not-short-circuited.md - `?:` computes
  BOTH arms unconditionally (no branch), so a `move` inside either arm always runs
  regardless of the condition, and a dereference in the unselected arm can still crash.
- internal/issue/ternary-int-condition-fails-module-verification.md - a `?:` with a
  non-`bool` condition (e.g. `int`) fails LLVM module verification outright; a separate,
  narrower bug in the same area (the condition operand is never coerced to `i1` before
  `CreateSelect`).
- internal/issue/unique-pointer-reassign-via-move-loses-ownership.md - `k2 = move k;` (a
  plain `=` reassignment) leaks; `unique R* k2 = move k;` (decl-init) does not.

## Fix direction

A principled fix needs compile-time null-narrowing typestate for pointer locals: a
dominating `x != nullptr` (or `x == nullptr` in the negative branch) clears "known-null"
with an OR at each merge point, the same shape the reverted attempt used for
"maybe-moved" but keyed on provable-non-null instead. Once that exists, the same-block
gate on `ExplicitlyMovedNull` can be dropped and the flag re-checked generally. The
loop-carried case additionally needs the flag fed through the existing whole-module
`RunMoveDataflow` fixpoint, with `Use` events recorded ONLY at dereference sites (never
at plain reads) - mixing that into the existing `IsMoved` event stream would resurrect
the false positives above. Ties into internal/plan/ownership-transparent-assignment.md;
this is not a one-off patch.
