# A warm-cache template wrapper re-request loses the argument type's header

Found 2026-09-23 in review of fix/cpp-container-sink-gaps (macOS arm64, Release). Pre-existing:
the pre-fix binary reproduces it with a copyable key too. Compile error on valid code, and it
depends on cache order.

## Summary

Start from an empty CFLAT_CACHE_DIR. Compile a program with ONE `std.set<cplv.Key>.emplace(4)`
using `-o`. A later `--check` of a program with TWO emplaces then fails:
`no instantiation of C++ function template 'std.set$cplv.Key.emplace' accepts these argument types
(std.set$cplv.Key, int) (clang: use of undeclared identifier 'cplv')`.
The second wrapper request is parsed in the 'set' import group only. The group that declares the
key (`cpp_interop_lvalue_sink.h`) is missing. `collectTypeDependencies` in
RequestCxxFunctionTemplate (LLVMBackend_CInterop.cpp) derives the dependency groups from
`cxxTypeOwnerGroup_`. On a warm cache that map most likely has no entry for the key (not verified).

## Repro (two steps, fresh cache; `-i Test -i Test/library`)

1. `CFLAT_CACHE_DIR=<empty dir> x64/Release/cflat scratch/f4_m0.cb -i Test -i Test/library -o m0.out`: rc 0.
2. `CFLAT_CACHE_DIR=<same dir> x64/Release/cflat scratch/f4_m3.cb -i Test -i Test/library --check`: rc 1, the error above.

f4_m0.cb and f4_m3.cb are the reviewer's scratch/rv/m0.cb and m3.cb. Both import vector, set, map,
utility, library/cpp_interop_lvalue_sink.h, library/cpp_interop_assign.h and test_helper.cb.
m0 does `std.set<cplv.Key> keys; keys.emplace(4);`, and m3 adds `keys.emplace(2);`. The script
is scratch/f4.sh. Before the round-2 change, step 2 also failed on Test/test_cpp_interop_bridge.cb
(at 4432,8). That block now uses insert(temporary) and operator[] instead of emplace, and step 2
passes on it. The underlying defect remains.
Pre-fix binary: the reviewer reproduced it with a copyable key, with scratch/rv/pz6.cb -o and then
pz8.cb --check.

## Fix direction

Persist the type-owner group in the cache round-trip, or recompute it for cached records, so the
dependency groups of a re-request include the header that declares every template argument.
