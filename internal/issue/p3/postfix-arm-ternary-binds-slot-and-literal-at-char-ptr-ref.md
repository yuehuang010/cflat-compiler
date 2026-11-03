# Ternary arm lvalue leftover after T475: string-literal arms bind a C++ `const char*&` parameter

Found by T325 review 1 (2026-10-16), PRE-EXISTING on master. T475 (2026-10-24) fixed the scalar postfix arm
(`ibump(t ? i++ : j)`, `t ? j : i++`, `t ? i-- : j` now refused like clang; legs Test/errors/err_cpp_ternary_prvalue_arm_lvalue_ref.cb).

## Fixed in MIX21 (round 1 and 2)

- `int* p = &(t ? i++ : j);` took the address of `i`. Now refused: "cannot take the address of a '?:' unless both
  arms are lvalues" (Test/errors/err_ternary_postinc_address_of.cb). `int** p = &(t ? x : y);` with two `int*`
  lvalue arms now names the selected pointer slot (Test/test_cpp_interop.cb, return 67054).
- `cbump(t ? "ab" : "cd")` and `cbump(t ? p : (u ? q : "x"))` (also through a nested `?:`) with a C++
  `void cbump(const char*& r)` are refused: "cannot bind a pointer rvalue" (Test/errors/err_cpp_ternary_literal_arm_pointer_ref.cb).
  Refusal is at LowerAliasByPointerArg, which follows incoming PHIs of the join.

## Fixed in TERNADDR

- (a) `r21.pr(t ? x + 1 : y)` into C++ `void pr(int*& r)` is refused: "cannot bind a pointer rvalue"
  (Test/errors/err_cpp_ternary_pointer_prvalue_ref.cb). A pointer join with a storage-less arm is marked
  `cflat.ptr.prvalue.join` in ParseTernaryBranches; LowerAliasByPointerArg reads that mark. Two int* lvalue arms still bind.
- (b) WRONG CODE: `&(t ? s1 : s2)` in a call argument passed the address of a COPY. Root cause: the join
  storage (collapseLvalueArms / collapseLvalueSink) was derived only from the initializer and owning-record
  assignment parents, so a `&` operand under a call argument, return or non-owning assignment got a value PHI
  and a stack temp. Scalar arms bypassed that through scalarLvalueJoin, which is why int arms worked.
  Fix: TernaryIsAddressOperand (now walks through `?:` arms) joins the storage of record and scalar lvalue
  arms (addressLvalueJoin); a record or scalar prvalue arm under `&` is refused like clang.
  Legs: Test/test_operators.cb ternaddrLegs (adr_* checks); Test/errors/err_ternary_prvalue_address_of_record.cb.
- (c) `?:` nested 41 deep SIGSEGV: Fixed in TERNADDR round 2 by a real stack-headroom check, not a step cap.
  RequireStackHeadroom (MainListener.h) runs at ParseConditionalExpression, ParseUnaryExpressionImpl and
  ParseStatement; InstantiateGenericFunction checks too. StackBytesRemaining (CompilerManager.cpp) reads the
  current thread's stack bounds (pthread on macOS/Linux, GetCurrentThreadStackLimits on Windows); a refusal
  "expression or statement is nested too deeply" fires within 256 KB of the low limit. The round-1 64-step
  counter and the 4000 parse-tree depth limit are gone (they refused master-compiling code). Legs:
  Test/errors/err_ternary_nesting_depth.cb; Test/test_operators.cb ternaddrDeepLegs.
- Round 2 also fixed: `(t ? s1 : s2).a = v` writes the selected record (memberReceiverJoin); `&(t ? x : y)` with
  arms of different types is refused (Test/errors/err_ternary_mixed_arm_address_of.cb); a 400-deep else-if
  now refuses instead of crashing.

## Open residue (TERNADDR)

- The ANTLR parser overflows the 8 MB stack at roughly 6000 nested parentheses; no guard runs before
  compilationUnit(). Fix direction: a nesting pre-scan on the token stream before the parse.
- Paren-only deep input overflows in the ForwardRefScanner walkers (see nested-ternary-compile-time.md residue).
- Accepted where clang refuses (pre-existing, master and fix alike): `&(t ? x : (int)y)` (same-type cast arm,
  a prvalue in clang); `(t ? a : mk()).f = 9` (clang: "expression is not assignable").
- Refused where clang accepts (pre-existing): `&(t ? c1 : c2)` on char globals into `char*`; both refuse
  `i8*` vs `char*`, clang compiles it.
- Compile time of nested `?:` grows about 2x per level on master (see internal/issue/p3/nested-ternary-compile-time.md).
