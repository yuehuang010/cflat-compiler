# A `?.` chain whose result is an owned string temp prints garbage

Filed 2026-08-21, found during review of the `fix/backtester-report` branch. Reproduces on
both `master` (measured against the main checkout's Release build) and this branch's fix commit - not caused or fixed by that branch.

## Repro

```cflat
struct Person {
    string name = default;
    move string label() { return "hi-" + name; }
};
extern int main() {
    Person person = default;
    person.name = "world";
    Person* p = &person;
    printf("[%s]\n", p?.label().data());
    return 0;
}
```

## Measured behaviour

Built Release on this branch (with the ternary-alias-sticky-flag fix applied):

```
[ &<garbage bytes>]
```

(non-deterministic garbage bytes after `[`, not the expected `[hi-world]`). Exit code 0 - no
compiler error, no crash; the program runs to completion and prints corrupted data.

`p?.get().toString().data()` is a newly reachable spelling of the same shape once a `?.` chain
can end on a chained accessor call - worth re-probing once that becomes common, but not
separately measured here.

## Root-cause hypothesis

The `?.` null-conditional lowering builds an owned string temporary as the chain's final value
(here, `label()`'s `move string` return) inside the conditional/merge control flow, then flushes
owned temps for the chain before the temp is consumed by the enclosing expression (`.data()` /
the `printf` argument). By the time the caller reads the buffer, it has already been freed,
so `.data()` returns a dangling pointer into freed memory - hence garbage, not a compile error.
This mirrors the general "owned temp flushed before its final consumer" family of bugs, but
scoped to the `?.` short-circuit lowering path specifically (see
`LLVMBackend_OwnershipTemps.cpp` / the whole-chain `?.` merge in
`MainListener_PostfixExpression.cpp` for the merge-block flush ordering).

## Fix direction

Do not flush an owned-temp mark for a `?.` chain until after the chain's yielded value has been
consumed by the surrounding expression - mirror the ternary/`??` arms' `FinishTernaryArm`-style
hoist-to-merge-block handling instead of flushing at the chain's own merge point. Needs a
regression case with `move`/owning-return methods at the end of a `?.` chain, asserting the
printed value (not just "compiles").
