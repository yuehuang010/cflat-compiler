# CFlat structs with non-relocatable C++ fields: leftovers after T528

T528 fixed array element state after explicit move (R3: `_ = move arr[i]` leaves a valid moved-from element, clang counts). The T149 and T417 fix rounds resolved the dictionary value-copy, parameter-return cleanup, and trivial defaulted copy-assignment items. The active items in this file are:

## Member-initializer spelling gap

`A() : arr() {}` on a plain CFlat struct is refused with "a base initializer is only valid in a [cpp] struct"; clang accepts this spelling. This is a syntax gap, not a crash.

## Discard of a conditional move (T528 review 2, pre-existing)

`_ = c ? move a[0] : move a[1];` on foreign C++ elements performs no transfer on master or after T528;
clang moves the selected element into a materialized temporary. The conditional result loses element
provenance before the discard gate (repros `scratch/rev_t528_sol_cases/ternary{,_false}` in the T528 review).

## Pointer-reached element discard (pre-existing)

`_ = move p[i]` through a raw pointer, a user raw-buffer field or a user generic buffer keeps master's
destroy-in-place; T528 changed only fixed-array and `T[]` view slots.

`*p = b` after release through a pointer remains untracked under the borrowed-pointer ruling.

## Memberwise-copy IR nit

`GetOrCreateMemberwiseCopy` adds a `%source` slot and recopies trivial fields for plain CFlat structs. Values match; the IR differs from the earlier output shape.
