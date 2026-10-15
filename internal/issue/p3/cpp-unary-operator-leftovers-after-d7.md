# Unary operator leftovers after D7+C3

Summary: T150 closed the member-vs-free ranking and binary candidate-reason items. These remain open.

## Still open

1. Rvalue receiver cells still pick the free overload where clang picks the member. Namespaced and global, 7 cells each, e.g. `mx_none_lref_rval`, `mx_const_lref_rval`, `mx_cvol_lref_rval` (scratch/t150 matrices). Cause: the final `CreateOverloadedFunctionCall(opName, {thisNV})` re-resolves by name, applying rvalue-ref preference to non-ref-qualified members and considering non-bindable `S&` frees. Fix direction: dispatch the decided candidate directly.

2. A C++ call returning `T&&` bypasses an overloaded `!`. `struct S { int operator!() const { return 7; } }; S&& xv(S&);` / `int a = !xv(x);` exits 0; clang calls the operator (7).

3. Binary member vs `T&` free tie on a mutable lvalue. `struct S { int operator+(int); }; int operator+(S&, int);` / `S x; x + 1;` picks the free overload (exit 2); clang reports ambiguity.

4. PRE-EXISTING (master too): a volatile operand still reaches a free overload clang refuses. 21 cells in
   scratch/rev_t150_sol_r2_tmp/ (e.g. `vol_c_val_vl`, `vol_c_cr_cvl`, `vol_cvr_cvrr_vl`): `S` with a
   non-volatile `const` member and a by-value or non-volatile reference free; `-vr(x)` on a `volatile S&` gives
   22, clang rejects (no matching constructor / invalid argument type). TryUnaryFreeOperatorOverload ignores
   volatile. A blanket rejection was tried in T150 r3 and removed: a converting or template constructor can
   copy a volatile lvalue (scratch/rev_t150_sol_r3_tmp/template_copy_member.cb, clang 22). Fix direction: reject
   only when clang's own construction of S from `volatile S&` (or reference binding) fails.
