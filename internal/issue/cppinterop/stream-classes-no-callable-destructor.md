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

1. [`cpp-virtual-base-constructor-unreachable.md`](cpp-virtual-base-constructor-unreachable.md) -
   the constructor of a class with virtual bases takes an implicit most-derived argument cflat
   does not pass. This is now a clean refusal; before the guard it was a crash.
2. [`stream-open-instantiation-error.md`](stream-open-instantiation-error.md) - `open`'s body is
   emptied by an error clang reported during instantiation.

Plus the two spike gaps that were always part of this story:
[`std-free-functions-and-globals-unreachable.md`](std-free-functions-and-globals-unreachable.md)
(`std.cout`) and
[`cpp-alias-template-types-unresolvable.md`](cpp-alias-template-types-unresolvable.md)
(the `std.ofstream` spelling).

## Acceptance

Unchanged: a `std.ofstream` local opens a file in `scratch/`, writes, and destructs at scope
exit, asserted in `Test/test_cpp_interop.cb`. Delete this file when that passes. The destructor
half is regression-covered by Section M41 there.
