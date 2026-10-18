# `new VB()` / `VB v;` of a C++ class with a virtual base is refused (by design)

Found 2026-10-15 by the T247 review (pre-existing on master 68f3b221e). Repro:
scratch/t290_tmp/p_new.cb (+ vb.hpp, oracle vb_oracle.cpp); T247 case scratch/repro_keep/t247_rev/cases/e_vb.cb.

Master: `new VB()`, `new VB[2]`, `new D()` (diamond) and a by-value local `VB v;` are ALL refused.
The local is refused too - the original report's "local path works" premise is wrong.

Root cause: a NON-template class with a virtual base gets layoutRefusal "uses virtual inheritance,
which is not supported yet" (CClangExtract.cpp:3864) and stays an opaque shell
(LLVMBackend_CInterop.cpp:22312). Every by-value use is refused by that gate on purpose (plan
cpp-direct-abi-interop.md M6 "Refused: virtual bases"; RejectUnsupportedCxxLayout
LLVMBackend_CInterop.cpp:22944; local decl LLVMBackend_VariablesAndIR.cpp:431). `new` hit the
generic size error because GetType returns the shell. Templates differ: a foreign template spelling
is sized as a clang blob and its layoutRefusal cleared (CClangExtract.cpp:3881-3900), so
`new VBaseArgs<int>(5)` works (test_cpp_interop.cb:6134).

Fixed on fix/t290-newvbase: `new` now reports the real refusal (MainListener_Expressions.cpp,
RejectUnsupportedCxxLayout before the generic size message). Leg: Test/errors/err_cpp_new_virtual_base.cb.

Open - needs a maintainer ruling before build:
- Allowing `new`/`VB v;` of a vbase class = blob storage sized by clang sizeof (as templates), the
  ctor placement-new thunk (CClangExtract.cpp:306-307 skips refused records) and lifting the
  by-value gate. Member access through the pointer (`p->k`, the repro's own line) is the same
  shape as the pinned err_cpp_virtual_base.cb (`VBaseMid* m; m.mv`), so the repro cannot pass
  while that pin stands. err_cpp_struct_unsized_field.cb and err_cpp_struct_virtual_base.cb pin
  by-value and derivation refusals too.
- Plan-sized per the sibling issue cpp-inherited-member-access-and-virtual-base-diagnostics.md.
