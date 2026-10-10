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
