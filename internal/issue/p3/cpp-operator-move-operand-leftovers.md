# C++ operator `move` operand: leftovers after D8

Summary: leftovers from the D8 report (2026-09-28) and its reviews. D8 binds `move d` straight to a `T&&` or by-value operator operand, the same as `f(move d)`. Probes: scratch/repro_keep/d8/, rv8/, t79/, chaincmp/ (main checkout).

Open (MIX16 run, 2026-10-30):
- Member template BINARY operator: `template<class U> P4 operator&(U&& o) const` with `a & P4(4)` or `a & b` is refused ("no overload of 'operator&'"); clang accepts. Compound `template<class U> T& operator&=(U&&)` is FIXED (MIX16).
  Root cause (MIX16): the gate in CClangExtract.cpp dropped member operator templates (no identifier); fixed to publish them. The compound path now requests the instance (MainListener_Expressions.cpp, cxxClassLeft block). The binary path (TryBinaryOperatorOverload) never requests a member template instance. Forcing the request there (receiver = lhs storage, operand = rhs with IsRvalue) makes the member wrapper visible in functionTable["operator&"] (isMemberCandidate true, params tp.P4, tp.P4), but overload ranking still rejects it; not traced further. Reverted from the change.

Not reproduced (MIX16), premise does not hold on master or fix:
- MIX16: not reproduced, shapes: by-value operator param with a ctor prvalue (`P3 operator&(P3)`, `c &= P3(4)`, block-scoped `P3 r = a & P3(4)`): mv/dt match clang.
- MIX16: not reproduced, shapes: class-result binary `P1 p2 = p & P1(4)` and `p & mkP1(4)`: mv/dt match clang.
- MIX16: not reproduced, shapes: `c &= move a[1]` on a Q1/MO array element: values match clang (9, 8, 7); reading the moved element or another element after the operator or after `takeMO(move a[1])` gives the same result (no move-tracking difference observable). Only an internal flag difference is left, unobserved.
- MIX16: not reproduced, shapes: ref-qualified `RQ`/`RQ3` `c &= d` and `c &= move d` (value 5 and 401 as clang). `RQ2` (only `RQ2&&` `&`-qualified) with a plain lvalue is refused by clang too (no viable overload); cflat refuses with "takes ownership ... pass move"; no "rvalue receiver" message reproduced.
- MIX16: not reproduced, shapes: `c &= (cond ? move a : move b)` with `K` (value 211 both, as clang).
- MIX16: not reproduced, shapes: `a = (cond ? move b : move c)` into the forwarding `template<U> operator=(U&&)` (returns 400 as clang).
- MIX16: not reproduced, shapes: `move a + b` with member `A::operator+(const A&) const` and free `operator+`: both report "use of moved variable 'a'" on a later read (no unchecked lhs seen).

Fixed here (MIX16): by-value compound result from a prvalue receiver used as an initializer (`K k = (mkK(10) *= 2)`) and the chained free compound into a copy-only class (`F g = ((f -= 0) |= 4)`): the prvalue-receiver operator call now runs under CxxAssignSretScope::EmitOperatorCall (MainListener_Expressions.cpp invokePrvalueOperator), so the result constructs into the declarator slot (mv 0 / cp 2 as clang). Legs: Test/test_cpp_interop.cb (cpp_move_operand_ops.hpp Cnt/Cpy).
