# `take(c ? default : x)` into C++: remaining wrong shapes (T391 partial)

T391 landed two narrow pieces (no cell where master == clang changed):
- a `default` arm joined by STORAGE into a C++ call argument (an unowned class temporary) is now destroyed
  at statement end behind a live flag (MainListener_Expressions.cpp, ParseTernaryBranches, before
  PropagateAliasValue): the c=1 leak is gone for bound wrapper by-value, member const&, `rr`, `two`.
- a bound C++ class parameter joins into the class itself (wrapper destination stripped) only for a bare
  `default` argument, or a const& parameter whose other arm is a live local of the parameter class
  (MainListener_PostfixExpression.cpp, `sameClassLocalOtherArm`).

Repros: scratch/repro_keep/box1016b/t306/t306r4/ and scratch/rev_t391_sol_probe.sh (h.h include form = the
wrapper path; a header under the importing file's tree binds the C++ function directly, which is already right).
Values: value*1e7 + ctor/copy/move/ca/ma/dtor/live digits; master (clang), c=0 / c=1.

Still wrong:
- Bound wrapper by-value `take(c ? default : tw)`: 1100020 / 2100030 (clang 1100020 / 2000020). The lvalue join
  picks the copy wrapper, so the default arm is copied, then destroyed. Joining into a class prvalue (r2) costs
  an extra move at c=0 (1110030, a master==clang cell), so it was dropped. Needs an arm-dependent wrapper
  (move wrapper for the default arm, copy wrapper for the lvalue arm) or construction into the wrapper slot.
- `rv.rr(c ? default : tw)` with `rr(T&&)` + `rr(const T&)`: master overload kept (const&, 2001000010 /
  2002000020; clang 1001100020 / 1002000020). The bound wrapper slot cannot tell `T&&` from by-value.
- UNBOUND free function (first call in the file): `take(c ? default : tw)` still leaks (2100021). The arguments
  are parsed before binding (no C++ call-argument state), so the join fix does not apply.
- Conversion-operator arm (`rv.Look lk; take(c ? default : lk)`, `tref(...)`): c=1 picks the wrong arm
  (41000010 vs 1000010). Needs a class-valued conversion helper (CxxConversionOperatorTo rejects a by-value class
  destination). The direct-binding form is refused ("'?:' arms differ in type").
- Nested `take(c ? default : (d ? default : tw))`: refused (clang 1100020 / 2000020).
- `take2(c ? default : tw, c ? tw : default)`: 2200040 (clang 2100030).
- C++ constructor const& `rv.Holder(c ? default : tw)`: 401000010 / 402000011 (clang 401100020 / 402000020).
- Member const& c=0 `h.member(c ? default : tw)`: 301000010 (clang 301100020, the prvalue copy).
- CFlat callee with a C++ class by-value parameter `cftake(c ? default : tw)`: leaks at c=1 (2100021).
- Wrapper cost (not ternary-specific): a by-value C++ class argument moves once more through the CFlat wrapper
  (`take(rettw())` and `take(default)` 1010020 vs clang 1000010).
