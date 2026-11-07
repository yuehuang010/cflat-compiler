# `new` pointer element depth >= 2 is refused, not carried

RULED 2026-11-04 (maintainer): the depth-2 cap (`PointerDepthCap`) was an initial implementation limit, not language
design - fix it: carry any pointer depth, spelled or generic-substituted, like C (`int***`, `new int**[2]`, `new T(v)` with
T = int** -> int***). Supersedes the box 10 premise below that the refusal is correct; options (a)-(c) are all dropped.
Test consequence: the "caps at 2 levels" err_ legs (12 in err_double_pointer_arg_single_pointer_param,
err_single_pointer_arg_double_pointer_param, err_using_alias_pointer_overcap) become value legs; core containers keep
`new T[n]`. Likely plan-sized (37 PointerDepthCap sites): scope the carrier before a run.

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

## Box 10 attempt (2026-11-03) - parked, needs a maintainer ruling

The general type model caps pointer depth at 2 (`PointerDepthCap`, MainListener.h; `int*** p` is refused), so the
refusal of `new int**[2]` / `new T*` with T = int* is correct and the "carrier" premise above is moot unless the cap is
lifted. The real defect is that a generic-SUBSTITUTED pointee does not count toward the cap: `new T(v)` / `new T[n]` with
T at depth 2 silently produce a depth-2 result. Counting it (one depth predicate for spelled and substituted pointees)
works for user code (60-cell matrix green), but breaks the core containers: list / array / dictionary / hashset / queue /
stack / channel / spsc_queue / hpc parallel allocate `new T[n]` under `if const (is_pointer(T))`, and test_basic /
test_generics instantiate them with T at depth 2 (string-pointer / alias-pointer elements). Swapping those buffers to
calloc hit const-discard conversions (`void**` -> `(const char)**`) after three attempts. Decide: (a) lift the cap to 3
for `new` element buffers, (b) a core-only raw slot allocation builtin for pointer elements, or (c) leave substituted
`new T` uncounted. Implementer patch + 60-cell matrix kept in the main checkout's scratch/briefs/newdepth_*_parked.*
(not tracked).
