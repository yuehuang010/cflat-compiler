# Return keep walk: a callee that parks the argument in a global/out-param frees the arm temp early

Filed 2026-10-15 from the T234 review (PRE-EXISTING on master; use-after-free).
`int* globEarly(int* p) { g_keep = p; return g_keep; }` defined BEFORE its caller
`int* c1(int c) { return globEarly(c > 0 ? &(new Temp(13))->v : &fb); }`: the return flush frees the
arm temp and the caller reads freed memory (val=0 de=1, master; want val=13 de=0). Same with
`out->p = p; return out->p;` through a pointer parameter. Repro: scratch/repro_keep/rev_t234_globEarly.cb.

Root cause: ValueMayReachReturn's store branch only follows a value parked in a stack slot; a store to a
global, argument or heap is ignored, so a callee that reads it back and returns it looks non-reaching.
T234 made the DEFERRED proof (later-defined callees) treat such a store as reaching; the immediate walk
for already-emitted callees did not change. Fix: same rule in the immediate callee walk
(ParameterMayReachReturn -> ValueMayReachReturn with pendingReturnValue == nullptr), then check
test_collection_leaks frees stay as before (keep = leak, never an early free).

Also (T234 review, non-blocking): the core-cache save path (~LLVMBackend.cpp:7433) and the IR-view path
(~4639) do not call ResolveDeferredReturnKeepGates; a gate there would only leak on a warm cache.
