# Scan-time `if const` folder cannot decide `sizeof`, enum constants, casts

Filed 2026-09-03 from round-5 review of the alias-frames fix (q02 members 1+2, commit 4bfa4aa).

## Summary

The generic pre-scans (`ForwardRefScanner::ScanGenericTypeUses`,
`MainListener::ScanAndQueueGenericTypeUses`) decide a statement-level `if const` with the shared
`FoldCompileTimeInt` folder so a `using` in the TAKEN arm binds for later generic uses. The main
pass decides with `DecideIfConstCondition`, which accepts more forms. Where the folder gives up,
neither arm's alias binds and a later `Box<P>` fails with `cannot find the type 'P'` even though
the program is otherwise valid. Not a regression: master errors on the same probes (it never
bound body-level aliases for generic arguments at all).

## Repro

`scratch/q02m1_rev5_c05.cb` and siblings (c06, c11, c12, c15, f02, g03, g04):

    if const (sizeof(int) == 4) { using P = double; } else { using P = float; }
    Box<P> b;                          // cannot find the type 'P'

Forms that fail: `sizeof(T)` in any position, `MyColor.Red == 0` (enum member), `(int)1` (cast),
a `const int K = sizeof(int) * 2 - 7` used as the condition, and `false || sizeof(...)`,
`0 * sizeof(...)` (leaf gap propagates through the chain). Known-false short circuits
(`0 && sizeof(int)`) fold fine.

Forms that fold: literals, `__MACOS__`/`__WINDOWS__`/`__PLATFORM__`, `!`, `&&`, `||`, ternary,
arithmetic/comparison chains, `const int` globals, enum-free identifiers.

## Root cause

`cflat/MainListener.h` ~535-556 (`FoldCompileTimeInt` / `FoldCompileTimeIntLeaf`): the unary
fallthrough rejects `sizeof`, the cast production is not folded, and a postfix expression with a
member access returns nullopt instead of consulting the enum table.

## Fix direction

Teach the folder the three leaves the main pass already evaluates (`sizeof` via the backend's
type size query - both `ParseDeclarationSpecifiers` copies are NOT involved, the type name is
resolved through the existing alias/struct lookup; enum member via the enum registry; C-style
cast as a no-op on integer constants). Keep it structurally mirrored to `EvalIfConstConstant`;
add legs to `testFunctionScopeAliasAsGenericArgument` in `Test/test_generics.cb` for a
`sizeof`-decided and an enum-decided arm. The undecidable fallback (both arms in a throwaway
frame) stays as the safety net.
