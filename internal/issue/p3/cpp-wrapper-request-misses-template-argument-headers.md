# C++ wrapper requests omit the headers that declare template arguments

Found by T70 round 2 (2026-10-07); master same. Some generated C++ wrapper requests (explicit move
transfer `EmitCxxExplicitMoveTransfer` LLVMBackend_CInterop.cpp ~10360, generated move ctor, default
ctor, implicit dtor) build their TU from the template IMPORTER's header group only. For
`std.unique_ptr<cppon.Cls>` that drops cpp_interop_opnew.h (declares `cppon`), so clang reports
"use of undeclared identifier 'cppon'". Hidden in the default incremental executor; reproduces with
`CFLAT_CPP_INCREMENTAL=0` on scratch/repro_keep/t70_unique_repro.cb (run from a worktree root with
`-i Test`, fresh CFLAT_CACHE_DIR). A cached failure of this request then replays across builds.
Experimental partial fix: scratch/repro_keep/t70_groupfix.patch (fixes the move-transfer site; the
implicit dtor site fails next) - the fix is a shared "header group of a type incl. its template
arguments" used by every wrapper-request site.

UPDATE 2026-10-11 (T110 round 3): a shared MakeCxxClassRequestGroup (owner group + the groups of
every template-argument owner, via CollectCxxTypeOwnerGroups) now builds the TU at 17 class-owned
wrapper request sites (default/move ctor, xvalue transfer, brace ctors, variadic ctor, operator->,
type-definition upgrades, special-member completion, base conversion, refused-member retry, xvalue
pointer conversion); header cache 240. The default incremental executor compiles and runs
t70_unique_repro.cb cold and warm, and test_cpp_interop cold + second compile (it was the warm
failure that blocked T110). STILL OPEN: with CFLAT_CPP_INCREMENTAL=0 the implicit
std.unique_ptr<cppon.Cls> destructor still fails cold and warm ("no destructor cflat can call");
three targeted group updates (special-member completion, refused member retry, registered-type
definition upgrade) did not change it - see scratch/repro_keep/t110/report_r3.md.
Also still on the owner-only group (T110 review 2): the C++ member-type request
(LLVMBackend_Interfaces.cpp ~297, `std.unique_ptr<cppon.Cls>.element_type` / `std.vector<cppon.Cls*>
.value_type` -> "has no member type named", same on master; fix: MakeCxxClassRequestGroup(group, owner)),
and, not shown reachable: Overloads.cpp ~1790 (class pointer -> void* xvalue), EmitAndLink.cpp ~2857 and
CInterop.cpp ~6629 (assumed-body owners), CInterop.cpp ~17457/~17491 (field binding), ~19686/~24176
(base/member specializations). Probes: scratch/repro_keep/t110/rev2/.
