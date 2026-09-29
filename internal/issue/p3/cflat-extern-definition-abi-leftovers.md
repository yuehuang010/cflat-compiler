# CFlat `extern` definition C ABI leftovers after B7

(The return-ext item landed in 2cd2bb58, 2026-09-29; only the function<> item remains.)

Summary: from the B7 round-2 review (2026-09-28). B7 adds signext/zeroext only to body-less C declarations, and passes arm64 structs over 16 bytes as a caller-copy pointer.
- Calling a CFlat `extern` definition that takes a struct over 16 bytes through a `function<>` value segfaults. Master gave a silently wrong result. The indirect call through `function<>` still passes the struct by value while the definition expects a pointer. Fix: have function<> calls to extern-ABI callees use the same arm64 classification, or refuse taking such a function as a `function<>` value.
Probes: scratch/repro_keep/b7r2/.

B10 (2026-09-29) measured the function<> item. Sub-fix 3 was not landed:
- `function<> f = cf_big;` (local bind, thunk, natural ABI) works on master.
- The crash happens only when the pointer comes back from C: an `ident(cf_big)` roundtrip, or any C-returned pointer to a C function taking a struct over 16 bytes (probes in scratch/repro_keep/b10/f3c.*). So the gap is general, not specific to extern definitions.
- Root cause: thin function<> values use the natural ABI when bound locally but the raw C ABI when they cross C, and the call site cannot tell which. Fixing it needs reverse thunks at every producer (generalising cxxFunctionPointerAbiPlans_ to native structs). That is multi-site and needs a ruling.
