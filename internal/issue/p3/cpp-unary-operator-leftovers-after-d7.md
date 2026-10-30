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

Items 3-5 (compound member-vs-free ranking, `<=>` tie text, template free with a double operand) landed OPBIN (ef83ffa02); items 6-7
already matched clang on master (refused ambiguous, verified by the OPBIN reviews). Pre-existing residue from the OPBIN reviews:
- A const receiver with both `operator+=(K&, int)` and `template<class T> operator+=(T&, int)`: clang picks the template (21); CFlat
  refuses ("rejected because the operand is const"), and the message leaks the internal `__cxx_free.` prefix.
- A template parameter named like an unrelated record (`struct Z; template<class Z> int operator+=(Z&, int)`) on a const receiver:
  refused, clang 61.
- `struct Tf { int operator+(int); }; x + 1.5` with only the member (or an unrelated namespaced operator+ template): refused with
  "cannot pass parameter 'b' of 'operator+' of type 'char*' from 'float'"; clang 11.
