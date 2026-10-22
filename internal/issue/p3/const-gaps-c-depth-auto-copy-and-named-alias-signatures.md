# Const gaps left after T346: C pointer-depth const globals, named const-pointer alias signatures

Severity p3 (pre-existing; master behaves the same). T346 (9fb804fd7) closed the static C++ member, indirect-call const& result,
construct_at-into-const and const static holder items of this file's predecessor.

1. c_depth_write.cb: C header `extern int* const* q;` - `auto x = q; *x = &n;` is accepted on master
   (`--check` PASS, link succeeds); clang rejects it. The direct `*q = &n` spelling is also accepted
   by master and rejected by clang. Clang reports `q` as `int *const *`. `MapRawGlobal` computes the
   exact middle-level const bit but only stores downward-closed masks, so this C global reaches codegen
   with no ConstMask. The auto inference site cannot copy a level the imported global already lost.
   Repros: scratch/repro_keep/box1020/c05b_r3_preexisting/ (cdepth.h, depth.h), and scratch/t348/.
2. (T346 r5 review) `using CP = (const int)*;` overloads `int f(function<int(CP)>)` / `int f(function<int(int*)>)` with a named alias
   PARAMETER component: thin function<> fails "cannot find the type 'int*'" on master and fix (clang picks 11/22; Lambda<> works).
3. (T346 r5 review) generic member signature with T = CP: the mutable-return overload (`function<alias int*()>`) resolves to the const
   twin (11), clang picks 22. Both resolver twins only normalize named pointer aliases in RETURN components (T346 r4) - parameters and
   generic substitution keep the alias spelling.
Fix direction: item 1 = preserve exact C-global ConstMask levels in `MapRawGlobal` (R11 keeps only downward-closed masks as a CFlat
spelling - carry C exact levels like C++ ExactTopLevels; confirm with the const plan R11 text before building); items 2-3 = normalize named pointer aliases in every sig component
(ResolveSigComponentConstScanner / ResolveSigComponentConstCodegen), then legs in Test/test_cpp_interop.cb near 99138.
