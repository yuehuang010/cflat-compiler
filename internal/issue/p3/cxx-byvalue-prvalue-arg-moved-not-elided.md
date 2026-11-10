# C++ by-value prvalue argument gets an extra move in template wrapper

Remaining discrepancy: the generated CFlat template wrapper moves its by-value C++ class
parameter into the imported C++ function-template specialization. Leg 9359 measures one move;
clang++ -std=c++20 measures none. The wrapper creates a local temporary from its by-value
parameter before forwarding it to the C++ call.

Three implementation approaches were tried against the focused repro; none changed the count.
Keep leg 9359 unchanged until a clean forwarding fix is found.

The following extra copies are pre-existing; current counts match master and remain balanced
(dead == born), but clang++ -std=c++20 elides one copy in each case:

- A non-direct callee returning a conditional: 2,2,0,3 (clang 2,1,0,2).
- A non-direct callee returning a field: 1,2,0,2 (clang 1,1,0,1).
- A recursive callee: 3,2,0,4 (clang 3,0,1,3).
- `mk2(make(1))`: 2,1,0,2 (clang 2,0,0,1).

## Left after TERNELIDE (daa3a99f, 2026-10-30 box 7; Opus reviews, repros scratch/repro_keep/ternelide/ w/x/y/z .cb + .cpp twins, w.hpp)
- Same-class prvalue conditional (and plain prvalue) into `gen<T>(...)`, `function<>` and `Lambda<>` calls: one extra move (indirect /
  generic call path has no elision). Counts balanced.
- Hidden-friend and function-template `operator<<` with a prvalue/conditional lhs: one extra move (`k8 kmv8 hfr kd8 kd-8` vs clang
  `k8 hfr kd8`); same for a plain prvalue lhs.
- Compound assignment with a conditional rhs, `a += (k ? C(1) : C(2))`, is outside the operand gate (not probed against clang).
- `use(C(f(D(1))))`: D destroyed right after f returns (clang: end of full expression). MS ABI: `use2(k ? C(1) : C(2), b)` with a
  throwing copy ctor for b - the first slot is already handed to the callee, an unwind there leaks it (same as `use2(C(1), b)`).
