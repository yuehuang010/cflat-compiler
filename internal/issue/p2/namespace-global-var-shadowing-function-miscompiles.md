# Namespace name lookup leftovers after T187

T187 (2026-10-14) fixed Part A: inside a namespace a bare name resolves to the enclosing-namespace variable before a global
variable or global function (ParseIdentifier + VisibleVariableType order). Left open (all same on master):

- Same-scope pair: `int lo = 3;` and `int lo(int)` at global scope are both accepted (scratch/repro_keep/t182_rv2/q_dupglobal.cb);
  C/C++ refuse ("redefinition of 'lo' as a different kind of symbol"). Hook points: global variable registration
  cflat/MainListener_Declarations.cpp ~8789 (existing "redeclaration of global" check) and function registration in
  cflat/ForwardRefScanner.cpp; the function registry also holds imported C/C++/core symbols, so check origin. Before refusing,
  grep core/*.cb and Test/ for an existing pair.
- Mirror (crash): `int lo = 7; namespace N { int lo(int a){...} int g(){ function<int(int)> p = lo; return p(1); } }` exits 138:
  the global-variable lookup still runs before the namespace-function branch in ParseIdentifier. Call form `lo(1)` is right.
  Repro scratch/repro_keep/t187_rv1/c21_nsfn_vs_gvar_ptr.cb.
- Array-size constant: `const int K = 1; namespace N { const int K = 3; int[K] a ...}` sizes the array 1 (C++: 3) while
  `case K:` in the same namespace now gives 3. The const fold helper (cflat/MainListener.h ~1129) checks
  TryGetConstGlobalInt(name) first, but trying the namespace-qualified name first there was NOT enough (main session probe
  2026-10-14, still 1) - the array-dimension fold has another path; find it before editing. Repro scratch/repro_keep/t187_rv1/c09_const_switch.cb (33).
