# Unary operator leftovers after D7+C3

Summary: T150 closed the member-vs-free ranking and binary candidate-reason items; T275 (2026-10-15) closed the unary
rvalue-receiver and `T&&`-result items; T280 (2026-10-15) closed the binary member-vs-free tie and the binary rvalue
receiver. Items 1-2 are open; items 3-7 are pre-existing binary/compound/rewrite results found in review (T280 round 2-3).

## Still open

1. PRE-EXISTING (master too): a volatile operand still reaches a free overload clang refuses. 21 cells in
   scratch/rev_t150_sol_r2_tmp/ (e.g. `vol_c_val_vl`, `vol_c_cr_cvl`, `vol_cvr_cvrr_vl`): `S` with a
   non-volatile `const` member and a by-value or non-volatile reference free; `-vr(x)` on a `volatile S&` gives
   22, clang rejects (no matching constructor / invalid argument type). TryUnaryFreeOperator ignores
   volatile. A blanket rejection was tried in T150 r3 and removed: a converting or template constructor can
   copy a volatile lvalue (scratch/rev_t150_sol_r3_tmp/template_copy_member.cb, clang 22). Fix direction: reject
   only when clang's own construction of S from `volatile S&` (or reference binding) fails.

2. (T275 matrix) A const prvalue binds a free `operator-(C&&)`: member `operator-()` (non-const) + free `operator-(C&&)`,
   `-mkc()` with `const C mkc()` -> CFlat runs the free (2); clang: no viable operator. 3 global cells; the namespaced
   variant was not checked.

3. PRE-EXISTING (T280 round 2, found by the review, not fixed): compound assignment ranking. A member and a free
   `operator+=` on one lvalue are not ranked: `x += 1` with member `operator+=(int)` vs free `operator+=(S&, int)`
   gives 1 | 1 | ambiguous (clang); `cbConst(x) += 1` gives ERR | ERR | 2; `ccMk() += 1` (prvalue) gives ERR | ERR | 2.
   Cells: ca_lv, cc_lv, cb_clv, cc_pr in scratch/rev_t280/repro/. The T280 gate is `plainBinaryOperator`, which
   excludes `=` and the compound forms, so the compound path still uses master ranking.

4. PRE-EXISTING (T280 round 2): `<=>` rewrite tie. `x < 1` with a member and a free `operator<=>` on one lvalue
   gives 11 | 11 | ambiguous (clang). Cell: sp3_lt_lv.

5. PRE-EXISTING (T280 round 2): template free `operator+(Tf&, T)` with a double right operand. `x + 1.5` gives
   ERR | ERR | 21 (clang). Cell: tf_dbl_then_int.

6. PRE-EXISTING (T280 round 2): base member vs `GB&` free on a derived lvalue. `GD x; x + 1` with member
   `operator+(int)` in `GB` and free `operator+(GB&, int)` gives 1 | 1 | ambiguous (clang). Cell: gd_lv.

7. PRE-EXISTING (T280 round 3): owner-typed free on an inherited member. `struct B { int operator+(int) }; struct A : B {};
   inline int operator+(B&, int)`, `A a; a + 1` gives 1 | 1 | ambiguous (clang). The binary ranking pass filters
   out frees whose first parameter is not the receiver class (MainListener_Expressions.cpp, candidate loop), so
   this tie is not handled; owner ties are NOT handled by T280. Cell: owner.cb (owner.h).
