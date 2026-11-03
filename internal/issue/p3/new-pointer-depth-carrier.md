# `new` pointer element depth >= 2 is refused, not carried

Filed 2026-10-31 at the fix/newptrarr landing (residue of `new int*[2]` / `new int*(a)`).

## Summary

`new int*[n]`, `new int*(a)` and `new T*` with T = int (depth 1 after substitution) now parse and lower.
Spelled or substituted pointer depth >= 2 (`new int**[2]`, `new T*` with T = int*) is refused with the
new-pointer depth-cap diagnostic: the NamedVariable / TypeAndValue path has no carrier for a pointee that
is itself a multi-level pointer. A real carrier would lift the cap.

## Pre-existing, seen while verifying (master a9e82a96)

- Plain `new T(v)` with T = int** loses a pointer level (no diagnostic).
- `new const int*(&cx)` fails to parse.
- `int** pp; int* p = pp;` (int** -> int* store) is accepted silently.
- Data-pointer literal-0 accept legs at Test/test_basic.cb ~4028 and ~5462-5466 sit outside the
  2026-10-31 function-slot literal-0 ruling; data pointers still take literal 0 - confirm that is intended.

## Fix direction

Carry pointer depth for the `new` result (count of extra levels on the pointee) through
ParseNewExpression -> declaration / assignment sinks, then drop the cap in MainListener_NewDeleteMove.cpp.
