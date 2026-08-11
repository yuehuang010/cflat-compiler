# Invalid-IR family: use a Debug build to get a located assert instead of a verifier dump

Tracker, not a single defect. It records (a) the diagnostic lens for every issue in this repo
whose symptom is "module verification failed, rc 1, NO location", and (b) the candidate list to
sweep with it. Filed 2026-08-10 after `(void)expr` turned out to be one of these.

## The lens

The vcpkg LLVM debug libraries are built with assertions ON, so a **Debug** cflat aborts at the
IR CONSTRUCTION site instead of letting invalid IR reach the module verifier. Same input, both
configurations, measured on this Windows host:

```cflat
struct V {
    int n = default;
    void operator+(V o) { }
};
extern int main() { V a; V b; int r = a + b; return r; }
```

| Config | Result |
|---|---|
| Release | `Module verification failed: Invalid bitcast  %5 = bitcast void <badref> to i32`, rc 1, no location |
| Debug   | `Assertion failed: castIsValid(op, S, Ty) && "Invalid cast!", llvm\lib\IR\Instructions.cpp, line 3335`, abort, rc 3 |

The assert names the LLVM API that was misused. Attaching a debugger to the Debug binary gives
the cflat call stack that built it, which is the part the verifier dump throws away.

Note the assert comes from LLVM's own compiled `Instructions.cpp`, not just from an inline
header, so this is real LLVM assertion coverage and not only cflat's own `assert`s.

## Building Debug on Windows

`cmake_build.bat debug` FAILS on a 21.6 GB host:

```
c1xx: error C3859: Failed to create virtual memory for PCH
c1xx: fatal error C1076: compiler limit: internal heap limit reached
```

`cmake --build --preset` lets Ninja default to one job per logical core (20 here), and every
`cl.exe` maps the 683 MB PCH into its own address space - about 13.7 GB of mappings before any
compiler's working set. Cap the jobs:

```
cmake --build --preset win-x64-debug -- -j 4
```

Release survives the default on a near-identical 678 MB PCH because its per-TU state is smaller.
The PCH is large because it carries the LLVM headers, so shrinking it trades against build time;
capping jobs is the cheap fix. Not fixed in `cmake_build.bat` - it is a build-infra call.

## Why the test suite cannot find these

`test.bat` only exercises constructs the compiler ACCEPTS. Every issue below is a construct that
is currently rejected or miscompiled, so no test reaches it. A green suite - in either config -
says nothing about this family. Confirmed 2026-08-10: `test.bat Debug` passes 45/45 with
assertions active, while the repro above still aborts.

## Candidates to sweep

These files mention a verifier failure, an invalid cast/bitcast, or an unterminated block. The
list is by TEXT MATCH - it is the sweep input, not a claim that each one asserts. Only
`void-binary-operator-overload-...` has been confirmed to abort under Debug.

| Pri | Issue |
|---|---|
| p2 | `auto-binding-of-fixed-array-loses-shape.md` |
| p2 | `char-array-from-string-literal-has-no-spelling.md` |
| p2 | `function-type-as-generic-interface-type-argument.md` |
| p2 | `lambda-expected-type-leaks-into-nested-literal.md` |
| p2 | `lambda-literal-param-default-invalid-ir.md` |
| p2 | `return-value-type-mismatch-fails-module-verification.md` |
| p3 | `core-bitcode-may-cache-bodyless-rebox-thunk.md` |
| p3 | `discard-position-not-threaded-through-parens-and-ternary.md` |
| p3 | `error-inside-a-coalesce-arm-leaves-an-unterminated-block.md` |
| p3 | `expect-error-leaves-outer-nullcond-block-unterminated.md` |
| p3 | `indirect-call-string-to-charptr-fails-in-verifier.md` |
| p3 | `insert-block-liveness-not-audited-repo-wide.md` |
| p3 | `pointer-operand-of-shortcircuit-emits-invalid-icmp.md` |
| p3 | `simd-array-view-decl-verifier-failure.md` |
| p3 | `union-destruction-residues.md` |
| p3 | `void-binary-operator-overload-result-consumed-fails-verifier.md` (CONFIRMED aborts) |

## Sweep procedure

For each file: run its recorded repro against `x64\Debug\cflat.exe`, and append to that file the
assert text plus the LLVM source line. Group by assert site - several are expected to share a
root cause. Do NOT fix from this file; fix in the individual issue, and delete that file when
it closes.

## Why this is worth doing (the `(void)expr` precedent)

`(void)expr` was in this family and cost a day of CI time. The chain, recorded because it is
likely to repeat:

1. `ParseCastExpression` had no `void` destination arm - an ordinary missing case.
2. The non-constant leg was found, diagnosed exactly, and FILED as p3 rather than fixed.
3. The compiler then grew a workaround at a distance for its own missing case: the return path
   learned to treat LLVM's token `none` as void-ness (`isTokenTy()` in `EmitReturnExpression`)
   to absorb what the missing arm emitted.
4. Tests were then written asserting the workaround's behaviour, which converted "unfixed" into
   "specified".
5. The constant leg was UNDEFINED BEHAVIOUR that happened to work on macOS and hard-crashed the
   compiler on Windows (access violation, no output at all, not even the CompilerManager dump).

The lesson is not "test more" - the construct WAS tested, and the tests passed on the platform
they were written on. It is that an unfixed invalid-IR hole attracts workarounds, and a
workaround plus a test looks exactly like a feature. The `isTokenTy()` clause was removed once
the real arm landed; it was mutation-proven dead (suite green without it).
