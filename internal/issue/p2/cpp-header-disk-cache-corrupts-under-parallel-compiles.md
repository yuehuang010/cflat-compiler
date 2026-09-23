Bucket: p2 (build reliability; observed once 2026-09-23, not yet reproduced deliberately)

# Parallel compiles sharing the same C++ imports corrupted the header disk cache

While probing on fix/owning-view-reassignment-release, ~20 concurrent `cflat` compiles of small
programs that all import the same Test/library C++ headers left `x64/Release/.cflat/cheaders` in
a state where later compiles failed to link: `missing ___cflat_vthk_..._cflat_inc_31_thk`
(a vtable-thunk symbol the cached entry claims is emitted but no companion module provides).
Moving the directory aside and compiling cold fixed it. test.sh runs the suite in parallel with
a warm second pass, so the hazard is live for the suite too (one `test_cpp_interop_template`
cold-compile failure was seen during the same parallel burst and passed alone).

Fix direction: find the cheaders write path (cflat/LLVMBackend_StateAndImports.cpp, the sidecar
+ companion-module writes) and make it atomic per entry (write to a temp file in the same dir,
fsync, rename; readers validate the sidecar against the module hash); a reader that finds a
half-written entry treats it as a miss. Repro attempt: loop `for i in 1..20; cflat probe.cb -o ... &`
on a cold local cache and grep the links for `_thk`.
