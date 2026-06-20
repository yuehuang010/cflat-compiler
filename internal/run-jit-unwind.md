# --run multithreading and Windows SEH unwind under the in-process JIT

How `--run` (in-process LLJIT, `LLVMBackend.h::JitRun`) was made to execute multi-threaded
programs, and what it takes to register x64 unwind info for JIT'd code on Windows. Prototype
landed 2026-06-20; one Release-only gap remains (see "Open gap" and `internal/issue/run-jit-threading.md`).

## TL;DR of the design decision
You do **NOT** need ORC `COFFPlatform` or the ORC runtime (`orc_rt`) for this. The vcpkg LLVM
18.1.6 build ships `COFFPlatform.h` but **no `orc_rt` archive**, and `COFFPlatform::Create`
*requires* an orc-runtime archive buffer - so that route is unbuildable here. The workable route
is the **JITLink** object-linking layer plus a plugin that calls `RtlAddFunctionTable` by hand.
(`SehRegistrationPlugin` already existed in the tree as dead code for exactly this; it was never
wired in. `--run` was using the default RuntimeDyld layer, which never registered `.pdata`.)

## Why threading "needed" a feature: the real blocker
Windows x64 is table-based SEH: every non-leaf function has `.pdata`/`.xdata` and the OS finds it
via the module's exception directory. JIT'd code is not a loaded module, so its unwind tables are
invisible unless registered with `RtlAddFunctionTable`. Without that, a hardware fault on a JIT'd
**worker thread** (e.g. the `program` construct wraps `main()` in an SEH catchpad for crash
isolation) is never dispatched to the catch and the process dies. Verified empirically: the
crash-isolation probe caught the fault (`exitCode=-1`) as an AOT exe but segfaulted under `--run`.
Plain non-faulting threads "worked" under `--run` only because they never consult `.pdata`, which
is why the old blanket "no threads under --run" guard was broader than the actual problem.

## The seven things JITLink-on-COFF needs (all in `JitRun` + `SehRegistrationPlugin`)
1. **Force JITLink.** `LLJITBuilder().setObjectLinkingLayerCreator(...)` returning an
   `ObjectLinkingLayer` (default on Windows is RuntimeDyld, which does not give a plugin access to
   the `.pdata` LinkGraph). Attach `SehRegistrationPlugin` to it.
2. **Flag override.** `setOverrideObjectFlagsWithResponsibilityFlags(true)` and
   `setAutoClaimResponsibilityForObjectSymbols(true)`. Without these, LLJIT pre-computes symbol
   flags from the IR while JITLink recomputes them from the linked COFF object; for weak/COMDAT
   constants (`__real@...`, `__xmm@...`) they disagree and ORC asserts
   `"Resolving symbol with incorrect flags"` (Debug) - the program had already run correctly, the
   assert fires at teardown, so it looks like a teardown crash.
3. **Define `__ImageBase`.** COFF/x86_64 lowering (`COFFLinkGraphLowering_x86_64::getImageBaseAddress`)
   resolves the image base by scanning `defined_symbols()` for `__ImageBase`; only if absent does
   it fall back to an external process lookup that fails (`JIT session error: Symbols not found:
   [ __ImageBase ]`). The symbol does **not** exist at PostAllocation - COFF creates it lazily
   during the PreFixup lowering. Fix: in a **PostAllocation** pass define `__ImageBase` as a
   `Scope::Local` symbol anchored to the lowest block. Then lowering finds it and bakes every
   `ADDR32NB` RVA relative to a base *inside* the JIT'd image (so the 32-bit delta fits).
4. **Stub the SEH handler.** `__C_specific_handler` resolves to ntdll, >4GB from the JIT'd code, so
   its image-relative `.xdata` reference overflows `Pointer32`
   (`relocation target "__C_specific_handler" ... is out of range of Pointer32 fixup`). Fix: in a
   **PostPrune** pass (before allocation, so synthesized blocks get memory) build an in-image
   8-byte pointer slot (`Pointer64` -> real handler) + a 6-byte `jmp *ptr(%rip)` stub
   (`x86_64::createAnonymousPointer` / `createAnonymousPointerJumpStub`) and repoint the `.xdata`
   handler edges to the stub. Collect the edges to repoint *before* creating the slot, so the
   slot's own `Pointer64` edge (which must keep the true ntdll address) is excluded - otherwise the
   stub jumps to itself.
5. **Register `.pdata`.** PostFixup: `RtlAddFunctionTable(block, count, base)` per `.pdata` block,
   `base = lowest block address` (same base the lowering used). Do NOT reverse-engineer the base
   from a relocation: lowering rewrote the `.pdata` edge addends to be base-relative, so the old
   `imageBase = target - storedRVA` heuristic yields 0.
6. **Sort each table.** Windows binary-searches each dynamic function table, so sort the
   `RUNTIME_FUNCTION` array by `BeginAddress` (in `getMutableContent`, before registering). LLVM
   emits `.pdata` in function order but JITLink may lay functions out differently. (In practice it
   came back already-sorted, but the requirement is real and cheap to guarantee.)
7. **Drop the guard.** `ReachesThreadSpawn()` is kept for diagnostics but no longer rejects.

## JITLink pass-phase ordering (LLVM 18) - the part that wasted the most time
- `PrePrune` -> prune -> `PostPrune` -> **allocate (final addresses assigned)** -> `PostAllocation`
  -> **resolve external symbols** -> `PreFixup` (COFF ADDR32NB lowering runs here) -> apply fixups
  -> `PostFixup` -> finalize (copy working memory to the executable address).
- Consequences: synthesize new blocks (the stub) at **PostPrune** (must precede allocation);
  define `__ImageBase` at **PostAllocation** (addresses known, before lowering reads it and before
  external resolution would fail on it); mutate `.pdata` content + call `RtlAddFunctionTable` at
  **PostFixup** (RVAs are final; `RtlAddFunctionTable` only stores the pointer - the OS reads it
  lazily post-finalization, so registering the to-be-final address is fine).
- `jitlink::Symbol::getName()` returns `StringRef` (cmp to `"__ImageBase"` is fine);
  `LinkGraph::makeAbsolute` erases the symbol from the external set; `orc::MemProt`'s bitmask
  `operator|` is NOT exported into `llvm::orc` here - OR the bit values by hand and cast.

## Status
- **Debug:** fully working - plain `thread<T>`, the `program` construct, the threadpool (21/21),
  and worker-thread SEH crash isolation all run in-process and exit 0.
- **Release:** plain `thread<T>` works; the `program` construct crashes (0xC0000005) on the worker
  *before* entering user `main`. By elimination it is NOT the SEH catchpad (gating it off doesn't
  help) and NOT the unwind registration (`CFLAT_JIT_NOSEH=1` still crashes) - it is the program
  trampoline's worker-side per-thread `BlockAllocator` / `__prog_tls` emulated-TLS setup, and it is
  layout-sensitive (Debug LLVM tolerates it, Release does not). See
  `internal/issue/run-jit-threading.md` for repro and the next diagnostic step (needs a debugger).
- No regressions: full `test.bat` and `test_hpc.bat` pass on Release; changes are confined to the
  `--run` path in `cflat/LLVMBackend.{h,cpp}` - the AOT/exe path (incl. its SEH crash isolation) is
  untouched.

## Diagnostics left in (env-gated, default off; remove when the Release gap closes)
- `CFLAT_JIT_DIAG=1` - prints `.pdata` registration details and an "invoking JIT main" marker.
- `CFLAT_JIT_NOSEH=1` - skips `RtlAddFunctionTable` (isolation switch used to prove the Release
  crash is not the unwind registration).

## General lesson for JIT'ing more Windows constructs
Anything that relies on the OS walking JIT'd frames - C++ EH, `__try`/`__except`, `RtlVirtualUnwind`,
profilers/debuggers - needs `.pdata` registered as above. `RtlAddFunctionTable`'s base and the
baked RVAs must agree, and the per-table array must be sorted. The COFF-specific edge kinds
(`Pointer32NB`) live in `COFF_x86_64.cpp`'s private namespace, not a public header - prefer
edge-set bookkeeping over referencing them by name.
