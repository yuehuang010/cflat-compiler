# Non-callable local name does not shadow generic function

Summary: A bare generic function call ignores a same-named non-callable local, parameter, or field.
Repro: Bare `id(3)` with an `int` local, parameter, or field named `id`.
Master vs expected: master calls the generic function; clang rejects the call because the non-callable name shadows it.
Found: T463 r1 sol review, 2026-10-24 box, PRE-EXISTING
