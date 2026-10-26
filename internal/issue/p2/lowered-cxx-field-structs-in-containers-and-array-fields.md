# CFlat structs with non-relocatable C++ fields: remaining items

The T149 and T417 fix rounds resolved the dictionary value-copy, parameter-return cleanup, and trivial defaulted copy-assignment items. The active items in this file are:

## Member-initializer spelling gap

`A() : arr() {}` on a plain CFlat struct is refused with "a base initializer is only valid in a [cpp] struct"; clang accepts this spelling. This is a syntax gap, not a crash.

## R3: array element state after explicit move

`_ = move arr[0]; arr[0] = b;` leaves the element state unknown at runtime. The foreign C++ class path is the same issue (`f2.cb` and `h2.cb` in `scratch/repro_keep/v1b/rv5/`).

`*p = b` after release through a pointer remains untracked under the borrowed-pointer ruling; it is not included in R3.

## Memberwise-copy IR nit

`GetOrCreateMemberwiseCopy` adds a `%source` slot and recopies trivial fields for plain CFlat structs. Values match; the IR differs from the earlier output shape.
