# C++ wrapper requests omit the headers that declare template arguments (remaining)

Fixed: C++ member-type requests (`std.unique_ptr<cppon.Cls>.element_type`, `.pointer`,
`std.vector<cppon.Cls*>.value_type`, `.pointer`, nested `cppt.ListTraits<cppon.Cls>.elem_type`)
build from `MakeCxxClassRequestGroup`, and a typedef of a pointer to a class is harvested as a
scalar marker (CClangExtract.cpp, pointer-to-record branch). Legs: Test/test_cpp_interop.cb
"Member typedef requests keep the headers of template arguments".

Earlier wrapper sites (T110): default/move ctor, xvalue transfer, brace ctors, variadic ctor,
operator->, type-definition upgrades, special-member completion, base conversion, refused member
retry, xvalue pointer conversion all use MakeCxxClassRequestGroup (kCHeaderCacheVersion 240).

## Still open: CFLAT_CPP_INCREMENTAL=0 calloc overload (blocks the noinc dtor check)

Re-measured 2026-11-18 (CACHE-KEY, master 496b9392e and fix identical):
- `import cpp "memory"; import cpp "cpp_interop_opnew.h";` +
  `std.unique_ptr<cppon.Cls> p = std.unique_ptr<cppon.Cls>(new cppon.Cls(7));` compiles and runs
  under CFLAT_CPP_INCREMENTAL=0 (exit 0); so does `list<int>` with only `import cpp "memory"`.
- Adding `import cpp "vector"; import cpp "cpp_interop_basic.h";` plus `import "list.cb"` and a
  `struct Holder { std.unique_ptr<cppon.Cls> p = default; }` still stops at core/list.cb:48
  "cannot choose which 'calloc' to call: calloc(u64, u64), calloc(unsigned long, unsigned long)"
  (noinc only; default mode exit 0). This is not a request-group defect.

Next step: fix the noinc calloc overload, then re-check the noinc Holder assignment and the
implicit unique_ptr destructor.

## Owner-only sites (audit)

CACHE-KEY converted every type-owner site to MakeCxxClassRequestGroup / CollectCxxTypeOwnerGroups
(class pointer -> void* in LLVMBackend_Overloads.cpp, assumed-body owners in
LLVMBackend_EmitAndLink.cpp, assumed-body field binding in LLVMBackend_CHeaderDemand.cpp, the
CRecords field/base/member-typedef sites, the CSignatures member sites), and those groups now carry
the owner request's dependency groups. Function-owner and per-import-group sites stay owner-only:
an import harvest has no dependency groups.

## Open: pointer typedef over an unpublished class-template specialization

`template<class T> struct Box { typedef Later<T>* later_ptr; }` with `Later<Cls2>` not yet published by
the CFlat request (a header inline body instantiating it does not count, e.g. `Box2<long>.later_ptr l = q.mkl()`, clang 7): the scalar binding is now REFUSED (master wording), not void*. Clang accepts
`useLater(l)` (value 0). Preferred fix: publish the leaf record for this request (complete it in the
probe and request it by name), then bind. Until then the member typedef is refused.
