# C++ ctor in expression position copies an exact-type lvalue instead of binding const T&

Pre-existing on master (found 2026-09-27 by the fix/ctor-thunk-ref round-7 review): with
`struct RW { long v; RW(const long& r, long* p) { *p = 9; v = r; } }`, `ee.RW(l, &l).v` gives 5 (a copy of l
was bound), clang gives 9 (r aliases l). The declaration form and `new` bind correctly. Also: an enumerator
passed to `T2(E&&)` / `T2(const long&)` is refused ("pass 'move'"); clang picks 1 (a prvalue enumerator binds
E&&). Probes: scratch/rev7_q/a/e.cb (main checkout; copied from the
fix/ctor-thunk-ref worktree). Fix direction: expression-position ctor args use the same lvalue-address path as the declaration form;
an enumerator constant is a prvalue for rvalue-reference binding.
