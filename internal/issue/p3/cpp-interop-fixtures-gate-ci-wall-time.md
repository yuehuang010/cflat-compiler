# The three C++ interop fixtures gate CI wall time (buildci warm 372 s vs 300 s target)

**Summary.** `buildci.bat` Release, warm request cache, 2026-09-26: 372 s green. test.bat 144 s
and the LSP sweep 150 s are both bounded by `test_cpp_interop_bridge.cb` / `_template.cb` /
`test_cpp_interop.cb` (108 / 118 / 80 s inside the parallel suite, 100 / 78 / 62 s in the LSP
sweep). Standalone warm, the bridge fixture compiles in 42 s; the rest is core contention.

**Profile** (`-ftime-trace`, bridge fixture, warm, self time): LinkCxxCompanion 12.7 s (290
companion bitcode modules linked into the program module with `Flags::None`),
CxxTypeRequestReplay 11.2 s (290 replays), ForwardRefScan 5.0 s, CodeGeneration 3.2 s,
RebindCxxCachedSignatures 3.0 s, CHeaderJsonConvert 2.8 s, TryLoadCxxTypeRequestCache 2.7 s,
CxxCompanionBitcodeParse 2.2 s.

**Tried and rejected.** `Linker::Flags::LinkOnlyNeeded` per companion, iterated until a pass
adds nothing: every pass re-adds the same 813 private globals (initializer thunks and literals
reached through the appending `llvm.global_ctors`), so it never converges and a second pass
duplicates static initializers. A single LinkOnlyNeeded pass is unsafe on its own because a body
linked from a later companion can need a helper an earlier companion defines.

**Landed 2026-09-26 (evening timebox).** Items 1 and 2 below: bridge fixture warm standalone
41.8 s -> 31.3 s (trace total); LinkCxxCompanion 12.7 s -> 4.9 s. Remaining profile (self time):
CxxTypeRequestReplay 9.8 s (290 replays; the scope spans everything after a cache hit - record
registration, signature rebinding, nested requests), LinkCxxCompanion 4.9 s, CodeGeneration
3.1 s, RebindCxxCachedSignatures 2.8 s. Bridge/template/interop warm: 33 / 46 / 18 s.

**Fix direction (ordered by expected payoff).**
1. DONE. Request-local promotion is lazy: `usedFunctionWork` entries are handed to CodeGen with
   `HandleTopLevelDecl` only, so a body is emitted exactly when a Phase 2 root reaches it.
2. DONE. Companions merge into one scratch module, then ONE `LinkOnlyNeeded` pass into the
   program. Note llvm::Linker drops an unreferenced linkonce definition when the destination
   does not name it - the scratch module pre-declares every linkonce definition before its blob
   (`DeclareLinkOnceDefinitions`) or later blobs' calls stay unresolved.
3. Replay cost: 290 x 34 ms after a cache hit. Profile inside the replay scope (record
   registration vs `RebindCxxCachedSignatures` vs nested requests) before touching it.
4. Make the request cache carry the merged per-group companion instead of 290 sidecars.
5. `test.bat` scheduling is a non-item: every worker starts at once, so order does not matter;
   the in-suite cost is core contention.

## Observation 2026-09-26 (request cache converges on the SECOND run, not the first)

Bridge fixture: cold run stores its entries; the next run still STORES 83 entries (4.6 s in
`StoreCxxTypeRequestCache`, 204 files rewritten under `cheaders/v102`); the third run stores
nothing. The second-run keys have no cold-run twin: the same C++ spelling (e.g.
`std::shared_ptr<__cflat_user::SharedLeaf>`) was attributed to a different OWNING import group
(`cpp_interop_sfinae.h` on the cold run, `cpp_interop_basic.h` on the warm run) with a
different request-source hash, so group attribution depends on which earlier requests came
from cache. Steady-state CI (cache persisted across runs) is unaffected; a freshly wiped
cache pays it once. Fix direction: make the owning group of a type request independent of
cache state (attribute by the header that declares the record, not by the request that first
reached it).
## Tried and rejected 2026-09-26: letting LSP analysis write its |EDECL request-cache entries

The LSP sweep (buildci's second-largest block, 144 s warm) never hits the request cache: LSP
mode looks up |EDECL keys that nobody stores, so every sweep re-harvests the three fixtures
through clang (58 / 73 / 97 s). Dropping the `symbolSink_` refusal in `StoreCxxTypeRequestCache`
and the two header-cache write gates made it WORSE: sweep 315 s on the writing run (20 pool slots
writing the same entries), 181 s on the next, and `test_cpp_interop_bridge.cb` fails from disk
with "'make_shared' is not a member of namespace 'std' (clang: use of undeclared identifier
'__cflat_user')" - a body-less LSP entry replays a generated-prefix dependency that its key does
not carry. Reverted. Any future attempt must first make the LSP request key carry the generated
prefix (or key the |EDECL entry on the same request source a compile would build) and must
throttle writes across pool slots.

