# C++ virtual-base pointer call passes the unadjusted derived address

Pre-existing on frozen master 33866560; also present on fix/t26-iosenum 1dce0314.

Repro: import a lazy C++ header with V { int value = 731; }, L : virtual V,
R : virtual V, D : L, R, an inline D object, L* left_ptr() returning &object,
and Sink::pointer(const V*) returning v->value. Call sink.pointer(left_ptr())
from CFlat. Clang C++20 returns 731; both compiler binaries return vptr bits.
Review artifacts: scratch/rev_t26_virtual.hpp, scratch/rev_t26_virtual_pointer.cb,
scratch/rev_t26_r2_virtual_pointer.cpp and scratch/rev_t26_r2_virtual_pointer.ll.

Root cause: a listed member call accepts the opaque pointer argument even though
IsCxxDerivedToBasePointer/FindCxxBaseOffset cannot establish a non-virtual offset.
No adjustment or refusal occurs: the unoptimized IR passes left_ptr()'s result
directly to Sink::pointer(const V*). LLVMBackend_Overloads.cpp:6160 only adjusts
when the helper succeeds, leaving this permissively matched call untouched.

Fix direction: preserve C++ overload ranking, then refuse the selected virtual-base
pointer conversion with LogError, or delegate/read the runtime vbase offset.
Do not drop a better candidate and select a worse overload instead.
