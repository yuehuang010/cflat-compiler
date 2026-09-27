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

**Fix direction (ordered by expected payoff).**
1. Shrink the companion modules at the source: a request's forced promotion (`usedFunctionWork`
   in `CClangExtract.cpp`) emits every `isUsed` specialization the request's own chunk reached;
   restricting that to what the request's ABI members call transitively would cut both the
   bitcode parse and the link.
2. Merge companions once: link all blobs into one scratch module (cheap, no big destination),
   then link that module into the program with LinkOnlyNeeded in ONE pass (no iteration, so no
   duplicate initializers; cross-companion helpers resolve inside the merged module).
3. Make the request cache carry the merged per-group companion instead of 290 sidecars.
4. `test.bat` scheduling: start the three fixtures first so they overlap the rest of the suite
   instead of finishing last.
