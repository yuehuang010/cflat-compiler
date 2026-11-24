# C++ wrapper requests omit the headers that declare template arguments (remaining)

Fixed: C++ member-type requests (`std.unique_ptr<cppon.Cls>.element_type`, `.pointer`,
`std.vector<cppon.Cls*>.value_type`, `.pointer`, nested `cppt.ListTraits<cppon.Cls>.elem_type`)
build from `MakeCxxClassRequestGroup`, and a typedef of a pointer to a class is harvested as a
scalar marker (CClangExtract.cpp, pointer-to-record branch). Legs: Test/test_cpp_interop.cb
"Member typedef requests keep the headers of template arguments".

Earlier wrapper sites (T110): default/move ctor, xvalue transfer, brace ctors, variadic ctor,
operator->, type-definition upgrades, special-member completion, base conversion, refused member
retry, xvalue pointer conversion all use MakeCxxClassRequestGroup (kCHeaderCacheVersion 240).

## CFLAT_CPP_INCREMENTAL=0 calloc overload: fixed (NOINC, fix box 16)

The calloc ambiguity was the fallback request TU re-harvesting libc's `::calloc`; see
internal/issue/p3/cpp-noinc-mode-divergences.md. Re-checked 2026-11-19 under CFLAT_CPP_INCREMENTAL=0
with `import cpp "memory"; import cpp "vector"; import cpp "cpp_interop_opnew.h";
import cpp "cpp_interop_basic.h"; import "list.cb";` and
`struct Holder { std.unique_ptr<cppon.Cls> p = default; };`: list<int> growth, the Holder
assignment `h.p = std.unique_ptr<cppon.Cls>(new cppon.Cls(7));` and the implicit unique_ptr
destructor compile and run (exit 0; master: calloc refusal, then "selected deleted operator '='").

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
