# Const C pointer-depth global gap

Severity p3 (pre-existing; master behaves the same). T346 (104f84360) closed the static C++ member, indirect-call const& result,
construct_at-into-const and const static holder items of this file's predecessor.

1. c_depth_write.cb: C header `extern int* const* q;` - `auto x = q; *x = &n;` is accepted on master
   (`--check` PASS, link succeeds); clang rejects it. The direct `*q = &n` spelling is also accepted
   by master and rejected by clang. Clang reports `q` as `int *const *`. `MapRawGlobal` computes the
   exact middle-level const bit but only stores downward-closed masks, so this C global reaches codegen
   with no ConstMask. The auto inference site cannot copy a level the imported global already lost.
   Repros: scratch/repro_keep/box1020/c05b_r3_preexisting/ (cdepth.h, depth.h), and scratch/t348/.
Fix direction: preserve exact C-global ConstMask levels in `MapRawGlobal` (R11 keeps only downward-closed masks as a CFlat
spelling - carry C exact levels like C++ ExactTopLevels; confirm with the const plan R11 text before building).
