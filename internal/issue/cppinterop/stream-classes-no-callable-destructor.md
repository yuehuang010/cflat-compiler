# Stream classes cannot be locals - destructor FIXED, two rungs remain

Found 2026-09-09 by the std header coverage spike
([`std-header-coverage-spike.md`](std-header-coverage-spike.md), gap 3).

## Status 2026-09-10

The destructor half is FIXED. `std.basic_ofstream$char.__dtor` and
`std.basic_ostream$char$char_traits$char.__dtor` now BIND, through an extern "C" thunk clang
emits (`cflat_cinterop::CxxMemberNeedsVirtualThunk` / `LLVMBackend::BuildCxxVirtualThunks`).
Verified with `cflat scratch/msabi_stream.cb --check -v`:

    C++ member __dtor dispatches through thunk
        '__cflat_vthk____D__basic_ofstream_DU__char_traits_D_std___std__QEAAXXZ'

The filed root cause was wrong. The destructor is neither implicit nor merely inline: it is
VIRTUAL, and MSVC's `basic_ostream` derives `virtual public basic_ios`, so
`MicrosoftVTableContext::getMethodVFTableLocation` reports a vfptr reached through a virtual base.
`CClangExtract.cpp` kept the slot index only for the primary vfptr and discarded everything else,
so the member was refused with "is virtual but cflat could not determine its vtable slot" - the
"no destructor cflat can call" message at the declaration site was the downstream symptom.

A directly named class with virtual bases never gets that far (layout refusal). A template
specialization does, because `nameOverride` clears the layout refusal and stores the class as a
sized blob - which is why the iostream hierarchy, and only spellings like it, hit this.

## What still blocks a stream local

Re-measured 2026-09-16 on macOS arm64 (libc++) after the virtual-base CONSTRUCTOR fix:

1. ~~the constructor of a class with virtual bases~~ FIXED. The measured `basic_ofstream<char>`
   and `ofstream` constructors now bind to clang-emitted placement-new thunks
   (`cflat_cinterop::CxxCtorNeedsVbaseThunk` / `CxxVbaseCtorThunkName`), so clang supplies the
   implicit most-derived flag / VTT. `std.basic_ofstream<char> f = default;` and
   `std.ofstream f = default;` both compile and run.
2. [`stream-open-instantiation-error.md`](stream-open-instantiation-error.md) - NOT reproduced on
   this host. `f.open("scratch/x.txt"); f.write("hello", 5); f.close();` compiles, runs, and the
   file contains `hello`; the destructor runs at scope exit. That issue was filed against the
   MSVC STL, so keep it open until a Windows host re-measures it.

So on macOS the acceptance below is behaviourally MET and only the assertion in
`Test/test_cpp_interop.cb` is missing. That leg was deliberately left out of the virtual-base
constructor change (out of scope) - write it, confirm on Windows, then delete this file.

Plus the spike gap that was always part of this story:
[`std-free-functions-and-globals-unreachable.md`](std-free-functions-and-globals-unreachable.md)
(`std.cout`). The `std.ofstream` spelling half LANDED 2026-09-16.

## Acceptance

Unchanged: a `std.ofstream` local opens a file in `scratch/`, writes, and destructs at scope
exit, asserted in `Test/test_cpp_interop.cb`. Delete this file when that passes. The destructor
half is regression-covered by Section M41 there.
