# A generated C++ wrapper's request key can omit the header its body needs

## Summary

Generated wrapper requests (`RequestGeneratedCxxWrapperUncached`, cflat/LLVMBackend_CInterop.cpp)
are keyed on `MakeCxxRequestGroup(primary, deps)`, and the incremental executor compiles them in the
primary group's shared clang TU. Most wrapper sites pass no dependency groups, and the ones that do
(`collectTypeDependencies`) look the argument types up in `cxxTypeOwnerGroup_`, which records only a
type request's PRIMARY group - the dependency groups that request needed are lost. So a wrapper can
compile only because an earlier request left another group's header in the TU, and its cache key
names fewer headers than its body uses. Type requests do not have this problem: their harvest is
filtered to the request's own include graph (`DropCxxDeclarationsOutsideRequestGroup`).

## Repro

Test/test_cpp_interop_bridge.cb line 5001, `true == bits[0]` on a `std.vector<bool>`: the
`__cflat_tpl_` wrapper is keyed on `cpp_interop_sfinae.h` alone, yet its body calls
`std::__bit_reference<std::vector<bool>, true>::operator bool` from `<vector>`. A clang TU of
`<new>` + `cpp_interop_sfinae.h` alone would reject it. Walking the wrapper body and refusing such a
wrapper (tried in the fix for cpp-request-cache-entry-carries-foreign-tu-signatures) breaks this
compile ("cannot bind its right operand ... non-const reference"), so it was not landed.

## Impact

Low: the replayed wrapper bitcode is self-contained and a compile only asks for it after resolving
the argument types, which needs an import group that declares them. The key is still wrong, and a
refusal-based guard cannot be added until the groups are right.

## Fix direction

Record the full request group (primary + dependency groups) with each type owner, and build every
wrapper group from the owners of its parameter and argument types. Then refuse a wrapper whose body
uses a declaration its roots do not reach (the include graph in `CxxIncrementalGroup` already has
what is needed: `ReachFromRoots` + a RecursiveASTVisitor over the wrapper body).
Acceptance: the bridge test passes with that guard live; test.sh stays green.
