# CFlat `extern` definition C ABI leftovers after B7

(The return-ext item landed in 2cd2bb58, 2026-09-29; only the function<> item remains.)

Summary: from the B7 round-2 review (2026-09-28). B7 adds signext/zeroext only to body-less C declarations, and passes arm64 structs over 16 bytes as a caller-copy pointer.
- Calling a CFlat `extern` definition that takes a struct over 16 bytes through a `function<>` value segfaults. Master gave a silently wrong result. The indirect call through `function<>` still passes the struct by value while the definition expects a pointer. Fix: have function<> calls to extern-ABI callees use the same arm64 classification, or refuse taking such a function as a `function<>` value.
Probes: scratch/repro_keep/b7r2/.

B10 (2026-09-29) measured the function<> item. Sub-fix 3 was not landed:
- `function<> f = cf_big;` (local bind, thunk, natural ABI) works on master.
- The crash happens only when the pointer comes back from C: an `ident(cf_big)` roundtrip, or any C-returned pointer to a C function taking a struct over 16 bytes (probes in scratch/repro_keep/b10/f3c.*). So the gap is general, not specific to extern definitions.
- Root cause: thin function<> values use the natural ABI when bound locally but the raw C ABI when they cross C, and the call site cannot tell which. Fixing it needs reverse thunks at every producer (generalising cxxFunctionPointerAbiPlans_ to native structs). That is multi-site and needs a ruling.

## Ruling (maintainer, 2026-10-09) - raised to p2 (segfault, still exit 139 on master)
`function<>` is a plain value (one code pointer, no room for metadata), and at runtime one variable
may hold either a C function pointer or a CFlat one, so the call site cannot pick a convention per
value. Rule: every call through `function<>` uses the C ABI. A C pointer (from C, a C struct
field, a callback param) is then callable as-is; a CFlat function whose address is taken into a
`function<>` gets a static C-ABI wrapper at that point (the compiler knows the callee there), as
`extern` CFlat functions already do today (`__cflat_abi_fnptr_<name>`). Only signatures whose
CFlat and C lowerings differ (e.g. arm64 struct > 16 bytes) need the wrapper; identical
signatures store the function directly. Repro: scratch/repro_keep/b10/f3c.cb + f3c.c (clang -9).
