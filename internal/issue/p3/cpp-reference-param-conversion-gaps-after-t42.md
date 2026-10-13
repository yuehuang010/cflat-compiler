# C++ reference parameters: conversions clang accepts that CFlat still refuses (after T42)

T42 (2026-10-05) made a C++ `const X&` / `X&&` parameter bind through a non-explicit converting constructor or a
non-explicit `operator X()`, with clang deciding viability (cached `X&& r = p0;` probe). The T42 Sol round-2 review
found three shapes clang accepts that CFlat still refuses (master refuses them too):

1. Template converting ctor from a NON-template class source: `template<class T> TC(const T& s)`, `tc(TC&&)` with a
   CS lvalue -> clang 617; CFlat "takes ownership ... pass 'move'". The pre-check's bothTemplateSpecializations /
   class-source fallback restriction denies it before clang is asked (LLVMBackend_CInterop.cpp ~26571, ~26589,
   ~26685, ~26719).
2. Inherited conversion operator: OpBase has `operator X()`, OpDerived inherits it -> clang `291 1 1` / 91; CFlat
   refuses. HasImplicitClassConversionOperator scans only source.members (~26642, ~26725, ~27257).
3. Reference-valued conversion operators `operator X&()` / `operator X&&()` -> clang binds the existing object
   (`1071 0 0`, `2071 0 0`, no temporary); CFlat refuses. Must bind the actual result, never copy/materialize.

Probes + clang oracles: scratch/repro_keep/t42_sol2/ (tc.*, baseop.*, basec.*, ref.*, refc.*, rref.*, rrefr.*,
axis.hpp; run.sh / extra.sh replay them from the T42 worktree layout).

## Fix direction

Let the pre-check admit these shapes and leave the verdict to the clang probe; lower reference-valued conversion
results as the bound reference.

## T104 run (2026-10-10)

Items 1 and 2 pass the added regression legs (617 for the class-template converting constructor; 291 with
one construction and one destruction for the inherited value conversion). Item 3a (`operator X&`) passes with
the existing object and no copy. Item 3b (`operator X&&`) remains unresolved after three failed attempts.

## T104 Retry 1 (2026-10-10)

Per retry instructions, all B3b (`operator X&&()`) implementation and coverage were removed; B3b remains open.
Items 1, 2, and 3a remain implemented and pass their focused regression legs. The T104 header still contains
the B3a reference source used by `Test/test_cpp_interop.cb`.

T104 review 2 (pre-existing, reproduces on master before T104): an inherited `operator X&()` through
a PUBLIC base is refused at a `const X&` / `X&` parameter while clang accepts (1071, binds the
storage). The B3a path finds only the class's own reference conversion operators. Repro
scratch/repro_keep/t104/rev_t104_ref_public.cb (+ .cpp).
