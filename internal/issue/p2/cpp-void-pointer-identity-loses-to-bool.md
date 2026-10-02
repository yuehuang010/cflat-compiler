# C++ overload set {void*, bool}: a void* argument picks bool

Found by the N54 round-3 review (2026-10-02), pre-existing on master.

## Repro

scratch/repro_keep/n54/vbool.cb (+ header in the same dir; oracle vbool.cpp): `vbool(void*)` = 3, `vbool(bool)` = 2; `void* v = ...; vbool(v)` returns 2 on master and after N54. clang returns 3: an identity (exact match) beats the pointer-to-bool boolean conversion ([over.ics.rank]).
Second shape, scratch/repro_keep/n54/mix.cb: a T* / void* / bool / nullptr_t set, int* call first, then void*: after N54 the void* call picks bool (master wrongly reused the int* template wrapper); clang picks void*.

## Fix direction

C++ overload ranking (LLVMBackend_Overloads.cpp, RankCxxConversionSequences / candidate matching): a void* argument at a void* parameter is an exact match and must outrank pointer -> bool. Check the same for T* -> T* vs T* -> bool.
