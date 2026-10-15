# Remaining C++ overload choice with a named fn-pointer lvalue (after T248)

T248 fixed the function-pointer overload ranking, parameter const propagation, and const move
cases. Remaining repros are in scratch/repro_keep/t214_rev/ with clang twins, unless quoted inline:
- Free `fsel(Fn&)` + `fsel(const Fn&)` with a const fn-pointer local is still refused "reference to a function
  pointer ... cannot bind" (clang 204; fixture t63.call_t248_free_pick). T248 (Luna) dropped the MapRawSig
  refusal and selected const Fn&, but the free-call lowering passed the code address, not the slot (SIGSEGV);
  backed out. Attempt: scratch/repro_keep/t248/t248_luna_full.patch (LLVMBackend_Overloads.cpp ~7259 hunk).
- Same WSel in call-argument position: `struct WSel { WSel(W a, int b); WSel(const int& a, const char*); }`,
  `int x = 0; int r = f(WSel(x, bump(&x)));` with `bump` = `++*p`, converting `W(int)`: 1111 (clang 1011).
  Local declaration is fixed; a by-value class ctor parameter in argument position still reads `x` late.
- Pre-existing, ruling question: `CharSel(char, int)` + `CharSel(const int&, const char*)` with `int x = 300;`
  and `CharSel(x, 1)` is refused "no constructor ... ('int', 'int')" on master and branch (clang 145).
  Likely the no-implicit-narrowing-at-call-args ruling (2026-09-04) rather than a defect; maintainer to confirm.
- (T232 review 2, pre-existing) a C-variadic ctor with an EXTRA variadic argument, `VarSel(x, bump(&x), y)`, gives 1111 (clang 1011): the const int& overload cannot take 3 args, so no selection helper is built and the by-value int goes by reference. Repro scratch/repro_keep/t232_rev2/.
