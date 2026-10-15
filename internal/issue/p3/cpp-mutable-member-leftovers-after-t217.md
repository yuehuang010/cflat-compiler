# Mutable C++ members still refused after T217 (inherited, union, array element, sub-struct member)

T217 (2026-10-14) made a `mutable` field of a const C++ object writable (direct, compound, postfix; constexpr
global emitted writable). The T217 Opus review found four shapes clang accepts that master and T217 still
refuse with "cannot assign to '...': it is a const C++ object" (repros + clang twins:
scratch/repro_keep/t217_rev/, script rev_t217_opus_probe.sh):
- inherited: `struct RDer : RBase` with `mutable int bm` in the base, `r_der.bm = 4` (clang rc 4)
- union member: `union RU { mutable int a; float f; }`, `r_u.a = 4` (clang rc 4)
- array element of a mutable array member: `mutable int a[2]`, `r_arr.a[1] = 9` (clang rc 9)
- member of a mutable sub-struct: `mutable RInner in`, `r_mi.in.x = 3` (clang rc 3)

Fix direction: IsCxxMutableFieldAccess (LLVMBackend_VariablesAndIR.cpp ~719) checks only the written address -
walk up parent field GEPs (stop at a load) and accept if any is mutable (array, sub-struct); propagate the
mutable flag to flattened base-class fields (LLVMBackend_CInterop.cpp ~22279); mark union member accesses.
Pin each with its refused non-mutable sibling.

Also pre-existing (same review): `r_cptr()->n = 6` through a `const RKM*` returned from C++ is accepted (clang
refuses); `*r_mp.p = 1` on `mutable const int* p` is refused correctly but the text says "const C++ object"
rather than naming the const pointee.
