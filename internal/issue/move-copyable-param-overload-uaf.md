# `V x = move <copyable by-value param>` steals the CALLER's buffer when a sibling `move V` overload + `alias K` param are present - heap use-after-free

Filed 2026-07-23 (found during STEP R3 of internal/plan/ownership-transparent-assignment.md
while probing typed-local release spellings; pre-existing bug, independent of R1/R2).

## Summary

Inside a generic method `bool tryAdd(alias K key, V value)` that ALSO declares a sibling
overload `bool tryAdd(alias K key, move V value)`, the statement `V refused = move value;`
on a copyable V (string) transfers the CALLER's real buffer instead of the param's local
copy/borrow - the caller's string is freed when `refused` dies, and the caller's later use
is a heap-use-after-free (ASan-confirmed). The same code is CLEAN with an `int key` (no
alias K), or without the sibling `move` overload, or with a bare `return false` instead of
the typed-local move.

## Repro matrix (scratch/, verified at R1+R2 working tree, ASan)

- scratch/r3_mock4.cb: alias K + sibling move-overload + `V refused = move value;` -> UAF
  of the caller's string buffer.
- scratch/r3_mA.cb: same but `int key` -> clean.
- scratch/r3_mB.cb: same but no sibling `move` overload -> clean.
- scratch/r3_mC.cb: same config, bare `return false;` (no typed-local move) -> clean.

## Root cause (hypothesized, not fully traced)

Overload-interaction misclassification: with the sibling `move V` overload registered, the
plain overload's `value` param appears to be classified as if it owned the caller's value
(move-param semantics leaking across the overload set, possibly via name-keyed rather than
signature-keyed param state, and only when the preceding `alias K` param shifts
positions/keys). `move value` then transfers the caller's buffer instead of copying or
erroring. Needs a trace through the overload registration + ApplyMoveParamTransfer state.

## Fix direction

Trace how param ownership flags are keyed across an overload set (functionTable entry
sharing?) and make the plain overload's param classification independent of its `move`
sibling; `V x = move <borrowed copyable param>` should either deep-copy-and-release or be
a compile error (ownership laundering), never transfer the caller's buffer. Add an err or
oracle regression leg from r3_mock4.cb when fixed. Secondary blocker for the R3 dictionary
collapse (primary: internal/issue/sink-param-thin-unique-exit-drop-gap.md).
