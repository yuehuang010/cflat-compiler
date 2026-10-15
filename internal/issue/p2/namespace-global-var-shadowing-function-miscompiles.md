# Namespace name lookup leftovers after T187

T187 (2026-10-14) fixed Part A: inside a namespace a bare name resolves to the enclosing-namespace variable before a global
variable or global function (ParseIdentifier + VisibleVariableType order). T193 fixed the mirror (namespace function used
as a value vs global variable) and the namespace-scope array-size constant. Left open:

- Same-scope pair: `int lo = 3;` and `int lo(int)` at global scope are both accepted (scratch/repro_keep/t182_rv2/q_dupglobal.cb);
  C/C++ refuse ("redefinition of 'lo' as a different kind of symbol"). Hook points: global variable registration
  cflat/MainListener_Declarations.cpp ~8789 (existing "redeclaration of global" check) and function registration in
  cflat/ForwardRefScanner.cpp; the function registry also holds imported C/C++/core symbols, so check origin. Before refusing,
  grep core/*.cb and Test/ for an existing pair.
- RULING NEEDED (T193, 2026-10-14): struct/class method bodies inside a namespace do not see the enclosing namespace
  (bare names take the global). C++ looks up the enclosing namespace (gives 11), but Test/test_generics.cb Leg K
  (`gf_method_body_generic` / `gf_method_body_plain`, ~4583-4602) pins the CFlat behaviour (10) as a CONTROL. T193 had
  the lookup working and reverted it to keep the pinned leg. Ruling: follow C++ (retarget Leg K to 11) or keep the limit?
