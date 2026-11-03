# C++ by-value prvalue wrapper and mixed-arm leftovers

The T239 parenthesized ternary init/return cases and operator-result declaration cases are
resolved. These out-of-scope discrepancies remain active:

- A template wrapper moves its by-value parameter before the C++ template call. Existing test leg
  9359 pins the extra move; clang++ performs no move.
- `rv.C kk = keep(rv.make(8))`, where CFlat `keep` returns its by-value parameter, has one extra
  copy at the declaration (1113 vs clang++ 1012).
- `{ l = c ? L(1) : move lv }` copies the move arm; clang++ moves it.

Root cause is not verified for these remaining cases. Fix direction: isolate the result object and
destination ownership at the CFlat wrapper boundary or mixed ternary arm, then compare counts with
clang++ -std=c++20 before changing the transfer path.

## Left after T239 (Opus review 1; repros scratch/repro_keep/t239_rev/, oracle.cpp + r.hpp)
- A unary C++ operator over a call, `rv.C z = -rv.make(4);` (also `-rv.pick(a)`, `-(a * rv.make(2))`), is refused
  "cannot initialize C++ class ... from this expression" (master too); clang accepts, 2,0,0,1. T239 r1 armed the slot
  for it and produced invalid IR (`sub ptr`), so a unary operand containing a call is not armed; arming it needs a
  ParseCxxFoldOperand scope around the unary operand in ParseUnaryExpression.

- (T246 review 1, pre-existing; repros scratch/repro_keep/t246_rev/cases/) subscript operands of a by-value operator
  keep one extra temp: `(vv[0] + b)` 1,2,1,3 (clang 1,2,0,2), `(vv[0] * b)`, `(vr[0] * b)`; `(-vv[1])` one extra copy,
  bare `-vv[1]` / `-vr[1]` refused; `auto z = k ? move a : C(2)` and `k ? (move a) : C(2)` still copy (paren hides move).

## Left after TERNELIDE (92c0054a, 2026-10-30 box 7; Opus reviews, repros scratch/repro_keep/ternelide/ w/x/y/z .cb + .cpp twins, w.hpp)
- Same-class prvalue conditional (and plain prvalue) into `gen<T>(...)`, `function<>` and `Lambda<>` calls: one extra move (indirect /
  generic call path has no elision). Counts balanced.
- Hidden-friend and function-template `operator<<` with a prvalue/conditional lhs: one extra move (`k8 kmv8 hfr kd8 kd-8` vs clang
  `k8 hfr kd8`); same for a plain prvalue lhs.
- Compound assignment with a conditional rhs, `a += (k ? C(1) : C(2))`, is outside the operand gate (not probed against clang).
- `use(C(f(D(1))))`: D destroyed right after f returns (clang: end of full expression). MS ABI: `use2(k ? C(1) : C(2), b)` with a
  throwing copy ctor for b - the first slot is already handed to the callee, an unwind there leaks it (same as `use2(C(1), b)`).
