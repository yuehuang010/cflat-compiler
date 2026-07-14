# Compiler state that fails to round-trip through the --init bitcode cache is invisible to test.sh

Created: 2026-07-14 (surfaced for the third time, during Phase 3 of internal/plan/optimistic-lock-coupling.md)

## Summary

`--init` pre-compiles the core `.cb` libraries to LLVM bitcode. Loading that cache **restores
compiler state from a hand-written serializer**, not from re-parsing source. Any field on
`TypeAndValue` / `StructData` / `AnnotationValue` that the analysis depends on but that is NOT
in the serializer is silently dropped on a warm cache. There is no error and no crash - the
feature simply stops working, and only for users who have run `--init`.

**The bug class is systematically invisible on macOS and Linux**, because:

| suite | runs `--init`? | catches this? |
|---|---|---|
| `test.bat` (Windows) | **yes** (line ~157; "a failure is a real regression") | yes |
| `test.sh` (macOS/Linux) | **no** - deliberate, per CLAUDE.md | **no** |

So the failure mode is: green on the primary dev machine (macOS arm64), red on Windows, and
discovered only after a push.

CLAUDE.md justifies the omission by calling `--init` "a bitcode-cache warmup; pure
optimization." **That rationale is wrong.** `--init` is a second, independent code path for
reconstructing compiler state, and it can lose information that the parse path preserves.

## History - this has now bitten three times

1. **`[Capability]` annotations** - `typeAnnotations_` is not serialized and is cleared on cache
   load, so a `--init`-warmed cache silently dropped every `[Capability]`. Recorded in
   `internal/plan/lock-capability-interface.md` as "Not in the original plan, and required".
2. **A residual hole in the same round-trip** - fixed in commit `8c207bf`, "Fix an issue where
   Capability list didn't survive --init".
3. **`TypeAndValue::LockThisMode`** (Phase 3, 2026-07-14) - a `lock(this.optimistic)` lambda
   parameter's MODE. Without an `ltm` key in the serializer, a warm cache would have demoted it
   to `Exclusive` and `err_lock_write_under_optimistic` would have stopped erroring. Caught only
   because the implementer thought to check.

Three for three, the same trap, in the same subsystem.

## Repro

```bash
# Cold cache - the guarded-write check fires correctly.
x64/Release/cflat Test/errors/err_lock_write_under_optimistic.cb -i Test/library   # exit 0 (errored)

# Warm the cache.
x64/Release/cflat --init

# Warm cache - if the new field is not in the serializer, the check silently vanishes
# and the expect_error test FAILS because the code now compiles clean.
x64/Release/cflat Test/errors/err_lock_write_under_optimistic.cb -i Test/library
```

`test.sh` never performs the middle step, so it can never see the difference.

## Root cause

The bitcode cache's serializer (`LLVMBackend.cpp`, the `TypeAndValue` / `StructData`
serialize+deserialize pair around lines 3750 / 3801) is **hand-maintained and additive**. Adding
a field to the struct does not add it to the serializer, and nothing on the POSIX test path
notices.

## Fix direction

**Preferred: make `test.sh` exercise the warm-cache path**, as `test.bat` already does. The
cheapest form is a second pass - run the `Test/errors/*.cb` suite (the expect_error tests, which
are the ones that detect *missing* analysis) once cold and once after `--init`. That is a small
number of fast compiles and it closes the whole class.

Failing that, at minimum update CLAUDE.md: `--init` is not "pure optimization", and any new
field on `TypeAndValue` / `StructData` that the analysis reads MUST be added to the
`LLVMBackend.cpp` round-trip in the same change.

Structural alternative (bigger): make the serializer exhaustive by construction - a single field
list that both the struct definition and the serializer are generated from - so omission becomes
impossible rather than merely discouraged.

## Note

Not a blocker for the optimistic-lock-coupling plan; `LockThisMode` was serialized correctly and
verified cold AND warm (all three `Test/errors/err_lock_*` tests pass in both states). This file
exists so the fourth occurrence does not happen.
