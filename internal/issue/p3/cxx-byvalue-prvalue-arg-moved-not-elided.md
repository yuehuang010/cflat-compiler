# Parenthesized ternary init and return residues (T223 review 1)

The main item (a C++ by-value parameter fed a prvalue call or ctor expression was move-constructed,
not elided) is RESOLVED by T231 and removed. Call results, ctor expressions, operator results and
derived prvalues now build in the parameter with clang's move/dtor counts (see
scratch/t231_matrix.md). This file keeps only the residues below, which T231 did not touch.

T223 review 1 (pre-existing, same on master; NOT re-measured by T231): a top-level parenthesized ternary
init `L x = (c ? L(1) : L(2))` (or `{ l = (c ? ...) }`) still moves once (clang 0); `return (c ? L(1) : L(2));`
is refused with "incompatible types 'pointer' and 'rv.L'" (clang accepts); `{ l = c ? L(1) : move lv }`
copies the move arm (clang moves).

## Left after T231 (review 1; repro scratch/rev_t231_opus_repro/ in the T231 worktree, kept at
## scratch/repro_keep/t231_rev/)
- RESOLVED by T235 (kept here for the record): prvalue LEFT operand of a by-value operator (2114 -> 2103,
  3014 -> 3003) and a call result next to a direct construction (`two2(make(3), C(7))` 2013 -> 2002: the
  constructor deferred into a clang wrapper, whose by-value parameter moved the prvalue).
- Declaration `Cnt z = l5 + make(8)` (and `Cnt z = make(2) + l5`): 2113 vs clang 2102 - one extra move and
  dtor from an OPERATOR result with a prvalue operand (`rv.C z = rv.C(3) + l5` 2124 -> 2113 after T235).
  A plain `rv.C z = l5 + l5` and `rv.C z = rv.make(8)` already match. Open: suspect the operator result is
  not recorded as the owned declaration source (LLVMBackend_Overloads.cpp ~7995 lastCxxRetTemp_); unverified.
- Template wrapper moves its by-value parameter before the C++ template call (leg 9359 pins it; clang 0).
- Pre-existing: `rv.C kk = keep(rv.make(8))` with a CFlat `keep` returning its by-value param: one extra copy
  at the declaration (1113 vs clang 1012).
