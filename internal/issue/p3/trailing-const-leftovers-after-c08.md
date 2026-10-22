# Trailing const leftovers after c08 (const plan phase 8)

Filed 2026-10-19 from the c08 implementer report (landed 87b36b2f1). Items 1-3 (#8 field naming,
#10 location for generic interfaces, receiver on internal interface dispatch) fixed in c11.
1. Calling a free function declared after the struct with `this` from inside a method (`pokeT(this)`) fails with
   "no overload matches", const or not (pre-existing, unrelated to const).
Design note (not a bug): `T Read() const { return v; }` with T = int* is refused because const is transitive through
pointer fields (phase 2); C++ is shallow. Keep unless the maintainer rules otherwise.

## Status 2026-10-20 (fix box)
Item 2 (alias result into this from an explicit const method) FIXED by T343. Item 1 remains:
- T343 r1 gated receiver suppression on "first argument is `this`" -> REGRESSION (Sol r1): method `S::poke(S*)` called as
  `poke(this)` refused; with a same-named free `poke(S*)` the free function was silently picked (clang 10, picks the method by
  class-scope name hiding). Pre-existing same root: `poke(self)` through a local pointer also fails.
- T343 r2 (per-candidate receiver injection, C++ name hiding) passed its matrix but broke test_program, test_cpp_interop_bridge,
  test_function_ptr (+3) after three attempts; parked patch + matrix + review repros:
  scratch/repro_keep/box1020/t343/ (t343_r2_parked.patch, t343_r2_matrix.md, rev_t343_sol_tmp/).
Fix direction: bare-call lookup inside a method body must be candidate-based (methods get the implicit receiver, free functions do
not, class-scope names hide namespace-scope ones like C++); start from the r2 patch and root-cause the six suite failures first.
