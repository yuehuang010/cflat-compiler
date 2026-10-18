# `take(c ? default : x)` into an unbound C++ free function picks the wrong arm / leaks

Found by T306 round 4 (2026-10-16), PRE-EXISTING on master. When the C++ free function is called for the first time
in the file (not yet bound, so no destination type exists while the arguments are parsed), a `?:` with a `default`
arm passed by value miscompiles:
- S1: `rv.Look lk = default; cpptw.take(c ? default : lk);` (Look: unrelated class with `operator cpptw::Twin()`) gives
  41000010 for both c; clang c=1 -> 1000010, c=0 -> 41000010 (wrong arm at c=1).
- `cpptw.take(c ? default : tw)` (tw a Twin local) leaks at c=1 (counters 2100021, live 1).
Once an earlier call has bound `take`, T306's argument-join path handles both. Repros:
scratch/repro_keep/box1016b/t306/t306r4/ (h.h with absolute includes; S1_samelayout_lvalue.cb; compile -i that dir,
run with and without an argument).

## Fix direction

Bind (or at least resolve the by-value class parameter type of) an unbound C++ free function before parsing its
arguments when an argument is a default-only `?:`, so the T306 destination-join applies; or refuse the shape until
bound. See cxxCallArgumentExpectedType_ in cflat/MainListener_PostfixExpression.cpp.

## More from T306 review 4 (master identical)
- const& param: `rv.tref(c ? default : tw)` leaves one live allocation at c=1; `rv.tref(c ? default : lk)` takes the wrong
  arm at c=1 (Look lvalue, conversion operator).
- Unbound state: plain `take(rettw())` gives 1010020 vs clang 1000010 (one extra move); S_opinner, X_fn_eq, T_dblcast
  one extra move. The header include spelling (relative vs absolute) decides the bound/unbound state, not call order.
