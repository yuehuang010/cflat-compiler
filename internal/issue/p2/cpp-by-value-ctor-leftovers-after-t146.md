# C++ ctor overload choice with a named fn-pointer lvalue (left after T146/T214/T232)

T232 fixed the local-declaration by-value snapshot (incl. default-argument and inherited ctors),
top-level const fn-pointer locals, and template/C-variadic ctors in argument position. Left
(repros scratch/repro_keep/t214_rev/ with clang twins, unless quoted inline):
- `T(bool)` + `T(Fn&&)` with a named fn-pointer lvalue picks the && overload (304, clang 501, boolrv_lv).
- `T(Fn)` + `T(Fn&&)` with a named fn-pointer lvalue is refused as ambiguous (clang 404, valrv_lv).
- Same WSel in call-argument position: `struct WSel { WSel(W a, int b); WSel(const int& a, const char*); }`,
  `int x = 0; int r = f(WSel(x, bump(&x)));` with `bump` = `++*p`, converting `W(int)`: 1111 (clang 1011).
  Local declaration is fixed; a by-value class ctor parameter in argument position still reads `x` late.
- Const fn-pointer PARAMETER: `int viaparam(const function<int(int)> p){ return take(Pick(p)); }` with
  `Pick(Fn&)` / `Pick(const Fn&)`: 104 (clang 204). MainListener_Declarations.cpp sets IsTopLevelConst
  only on the local-declaration path. (The const fn-pointer GLOBAL is fixed: 204.)
- `const Fn cfp = plus; Pick o = Pick(move cfp);` with `Pick(Fn&&)` present: 304 (clang 204; const Fn&&
  binds const Fn&).
- Pre-existing: free `fsel(Fn&)` + `fsel(const Fn&)` called with a const fn-pointer local is refused
  "reference to a function pointer ... cannot bind" on master and branch (clang 204).
- Pre-existing, ruling question: `CharSel(char, int)` + `CharSel(const int&, const char*)` with `int x = 300;`
  and `CharSel(x, 1)` is refused "no constructor ... ('int', 'int')" on master and branch (clang 145).
  Likely the no-implicit-narrowing-at-call-args ruling (2026-09-04) rather than a defect; maintainer to confirm.
- (T232 review 2, pre-existing) a C-variadic ctor with an EXTRA variadic argument, `VarSel(x, bump(&x), y)`, gives 1111 (clang 1011): the const int& overload cannot take 3 args, so no selection helper is built and the by-value int goes by reference. Repro scratch/repro_keep/t232_rev2/.
