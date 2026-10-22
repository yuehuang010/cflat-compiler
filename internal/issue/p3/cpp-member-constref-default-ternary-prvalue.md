# C++ member const& call keeps a local ternary arm as an lvalue

## Repro

In `Test/test_cpp_interop_bridge.cb`, `bridge_t391_ternref_member_cref(false)` calls
`T391Host::tref(const T391Val&)` with `false ? default : v`. The existing fixture is
`Test/library/cpp_interop_ternaryref.h`.

The `c=false` counter value is `3710010` on master and this worktree, while
`clang++ -std=c++20` reports `3711020`: the C++ conditional expression copies the
local arm into its result prvalue before binding the const reference.

## Root cause

`scratch/t395/t391_member_ir_prefix.txt` shows the member call receives
`%cxx_lvalue_storage_join`, with `%v` as the false incoming address and no copy
construction in that branch. The member-call argument path does not reach the
T391 class-valued conditional destination path used by the free const-reference
call. The particular gate that prevents it remains unresolved.

## Fix direction

Make the resolved C++ member parameter context reach the whole-argument ternary
join, so the default arm and local arm build one owned class prvalue slot before
the member call. Preserve ordinary named lvalue binding and member overload
selection. Do not key this decision on source text.
