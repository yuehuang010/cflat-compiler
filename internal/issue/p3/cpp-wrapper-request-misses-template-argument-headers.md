# C++ wrapper requests omit the headers that declare template arguments (remaining)

Fixed: C++ member-type requests (`std.unique_ptr<cppon.Cls>.element_type`, `.pointer`,
`std.vector<cppon.Cls*>.value_type`, `.pointer`, nested `cppt.ListTraits<cppon.Cls>.elem_type`)
build from `MakeCxxClassRequestGroup`, and a typedef of a pointer to a class is harvested as a
scalar marker (CClangExtract.cpp, pointer-to-record branch). Legs: Test/test_cpp_interop.cb
"Member typedef requests keep the headers of template arguments".

Earlier wrapper sites (T110): default/move ctor, xvalue transfer, brace ctors, variadic ctor,
operator->, type-definition upgrades, special-member completion, base conversion, refused member
retry, xvalue pointer conversion all use MakeCxxClassRequestGroup (kCHeaderCacheVersion 240).

## Still open: CFLAT_CPP_INCREMENTAL=0 implicit std.unique_ptr<cppon.Cls> destructor

Not reproduced in the WRAPGRP run (2026-11-06). The repro scratch/wrapgrp_repro/t70_unique_repro.cb
stops earlier, in both master and fix, at a separate noinc defect:

- core/list.cb:48 `calloc((i64)newCap, (i64)sizeof(T))`: "cannot choose which 'calloc' to call:
  calloc(u64, u64), calloc(unsigned long, unsigned long)". The first is cruntime.cb's wrapper, the
  second the C header's declaration. Any `list<T>` with CFLAT_CPP_INCREMENTAL=0 and a C++ import
  of memory/vector/basic.h hits it (`list<int>` too), so the no-dtor diagnostic is never reached.
- Noinc-only, master identical: `struct Holder { std.unique_ptr<cppon.Cls> p; };` then
  `h.p = std.unique_ptr<cppon.Cls>(new cppon.Cls(7));` -> "overload resolution selected deleted
  operator '='" (default mode passes).

Next step: fix the calloc overload first (or log the set of calloc candidates per import group),
then rerun t70 noinc. Only then root-cause the dtor with lldb or a GetOrCreateCxxClassDestructor
log (MainListener_Declarations.cpp, "it has no destructor cflat can call"). A candidate to test
first is LLVMBackend_EmitAndLink.cpp (upgrade of layout-only assumed bodies) builds its request
with the owner-only group, MakeCxxRequestGroup(groupIt->second, {}). Unverified.

## Owner-only sites (audit)

Not converted: no repro reached them in this run. The issue's old line numbers moved; current sites
are MakeCxxRequestGroup(..., {}) in LLVMBackend_Overloads.cpp:2878 (class pointer -> void*),
LLVMBackend_EmitAndLink.cpp:2921 (assumed-body owners), LLVMBackend_CHeaderDemand.cpp:1376
(assumed-body field binding), plus the CRecords/CSignatures/CxxRequest sites without a repro.

## Open: pointer typedef over an unpublished class-template specialization

`template<class T> struct Box { typedef Later<T>* later_ptr; }` with `Later<Cls2>` not yet published by
the CFlat request (a header inline body instantiating it does not count, e.g. `Box2<long>.later_ptr l = q.mkl()`, clang 7): the scalar binding is now REFUSED (master wording), not void*. Clang accepts
`useLater(l)` (value 0). Preferred fix: publish the leaf record for this request (complete it in the
probe and request it by name), then bind. Until then the member typedef is refused.
