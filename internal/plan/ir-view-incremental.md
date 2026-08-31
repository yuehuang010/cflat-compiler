# IR/Assembly view: incremental-edit roadmap

Goal: the user edits a file and sees the impact in the IR/asm/optimization views with
interactive latency. Status: the 2026-08-30 performance round (instrumentation,
memoized debug lookups, single-clone IR print, response/slot caching, root-file
scoping) took a view request from 1.5-2.9 s / ~2 MB on every file down to, for a
typical small file, ~60 ms first view / sub-ms repeats / ~3 KB payload. Large files
(test_basic.cb, 366 root functions) still pay ~0.5-0.7 s analyze on first view after
an edit plus 0.1-1.2 s emit depending on kind and opt level.

Measured request timings live in the response ("timings" object) and in per-request
stderr lines; scratch/measure_view_perf.py is the driver, scratch/check_root_subset.py
the faithfulness checker.

## Where edit-loop latency now goes

After an edit, the first view request pays:
1. analyze (full front-end recompile with debug info): ~40 ms small file,
   ~500-700 ms test_basic.cb. Traced breakdown (test_basic, warm cache): Analyze
   ~640 ms = Parse ~300 ms (of which lexing is only ~20 ms - the rest is ANTLR LL
   parsing) + CodeGeneration ~240 ms + ForwardRefScan ~45 ms + ProcessImports ~10 ms.
   User-import caching is therefore worthless; the parse and codegen walks are the
   levers.
2. emit: O0 ~10-100 ms; O2 pays the whole-module opt pipeline (~0.7-1.0 s on
   test_basic) plus codegen for asm.
Subsequent views of the same text are free (slot reuse skips analyze; identical
requests hit the response LRU).

## Candidate next steps, in rough order of value

Items 1-3 landed 2026-08-30 (second round): didChange refresh (400 ms client
debounce), shared on-save analysis (view-interested URIs analyze with debug info; an
in-flight table makes racing view jobs wait instead of re-analyzing - post-edit view
now pays emit only, 540ms -> 94ms small-file), and the per-(analysis, optLevel)
optimized-module cache (asm -O2 emit 1106ms -> 527ms, optinfo 1776ms -> 929ms on
test_basic.cb). Items 4-6 remain.

1. Refresh views on didChange (debounced), not only on save. Latency now permits it
   for small/medium files; the extension already debounces diagnostics at 250 ms. The
   response cache makes the no-op case (debounce fired, text unchanged) free.
2. Share one analysis between the on-save diagnostics run and the view refresh: when
   a URI has open views, run the debounced/didSave analysis with debug info on so the
   subsequent view job's slot-reuse check hits and the view pays emit only. Needs a
   client hint (extension knows which views are visible) or server-side tracking of
   URIs with recent view requests.
3. O2 emit cost: the opt pipeline is whole-module for fidelity (inlining of core into
   user code must be real). Options: cache the optimized module per text hash on the
   slot (opt once, emit ir+asm+optinfo from clones of it - today each request re-runs
   the pipeline on a fresh clone); or a documented "fast mode" that internalizes +
   GlobalDCEs before the pipeline (changes fidelity, so opt-in only).
4. Function-level incremental emit: key each root function's printed IR/asm block by
   a hash of its post-analysis IR; on edit, re-emit only changed functions and splice
   into the cached view text. O0 is straightforward (no cross-function effects);
   O2 needs conservative invalidation (a changed function invalidates its inliners -
   the optinfo instantiation graph already computes caller edges). Big win for large
   files where one function changed - which is exactly the edit loop.
5. Delta protocol to the extension: send changed line ranges instead of the full text
   so VS Code does not re-tokenize a megabyte-scale virtual document per save.
   (Root scoping already shrank typical payloads enough that this is deferred.)
6. Extension UX for the new server behavior: surface the "showing N of M functions"
   banner as a toggle (sends wholeModule: true) - DONE 2026-08-30 (two extra view
   choices, whole-unoptimized / whole-optimized) and status-bar timings (last refresh's
   analyze/emit ms, cached flag) - item complete.

## In progress: item 4, incremental -O2 rebuild (rounds 1-2, 2026-08-30)

Lives on branch feature/incremental-o2-wip; NOT on master. Design: snapshot
the optimized module as context-free bitcode (+ pre-opt StructuralHash per
function, global content hashes, name-keyed call graph, address-taken set,
remarks) in ResetForReanalysis - required because reset destroys the
LLVMContext; diff against the next analysis; re-optimize changed functions +
transitive callers + a DEPTH-BOUNDED callee closure (CFLAT_VIEW_INC_DEPTH,
default 2, "full"/-1 = unbounded); rebuild by llvm::Linker OverrideFromSrc
with dest = full clone of the NEW analyzed module and src = old optimized
snapshot with re-optimized bodies deleted (the reverse direction crashes:
erasing values with remaining uses is UB with assertions-off LLVM); protect
kept bodies with optnone+noinline through the pipeline; GlobalDCE after
stripping the protection so functions the full pipeline removes die here too.

Round-2 lessons, all verified by vscode-extension/test/measure_incremental_o2.py
- the committed LSP-driving equivalence driver (fixture:
vscode-extension/test/fixtures/demo_view.cb; run it standalone, not
concurrently with test_lsp.bat - both use the exe's local cache). The driver
compares the incremental view against
the SAME edit flow with CFLAT_VIEW_NO_INCREMENTAL=1: a fresh server is the
WRONG baseline because its first analysis is a different module composition
(see drift below). Its anti-vacuity rules (assert the probe edit changed the
view text, assert a [view-incremental] stderr line was seen, fail on compile
errors) exist because a prior driver silently reported no invalidation.

- llvm::Linker never merges internal/private symbols by name: kept local
  functions/globals from the snapshot get copied+renamed (.NNN) instead of
  overridden. Fix: temporarily promote named locals (kept pairs AND the work
  side of reopt'd locals, whose src half is only a declaration) to external
  around the link, restoring the WORK-side linkage after. One-sided named
  locals are safe (no collision). Unnamed private globals cannot be identity
  matched; their duplicates die in GlobalDCE and the residual renumbering is
  accepted (the driver normalizes it).
- Functions with no body in the snapshot (fully inlined+DCE'd by the old
  pipeline) cannot keep an old body: those transitively reachable as callees
  of the reopt set are re-optimized fresh; the rest keep the pre-opt body
  protected and die in the final GlobalDCE. A blanket "re-optimize all of
  them" balloons reopt past too-wide on test_basic (1016/1042).
- The address-taken guard applies to SEEDS only (their indirect callers are
  invisible to the name-keyed graph); callers/callees entering reopt via
  explicit edges are safe to re-optimize. Seeding every function containing
  an indirect call instead of falling back was measured and REJECTED:
  453/474 reopt on the small demo (main is address-taken; most core
  functions hold indirect calls). The precise alternative - inline-remark
  edges to find which kept bodies inlined the seed - fails today because
  cached core has no debug locations, so those inline remarks are dropped.
- test_basic-shaped files (mega-main directly calling hundreds of helpers)
  hit too-wide at ANY depth once main enters the caller closure, and that is
  honest: matching the full build there means re-optimizing most of what
  main inlines. The win is real for demo-shaped files: reopt 18/474, emit
  ~70ms vs ~93ms full, function sets identical (24/24) including a second
  consecutive edit.

FIXED (round 3): the test_lsp "viewAssembly: inline attribution" failure.
Mechanism: the server analyzes each document via a TEMP COPY whose path
changes per re-analysis, so kept snapshot bodies carry DIFiles pointing at an
OLD temp path while PrintModuleView's root detection compares against the
current analyzedRootPath_/compile unit - a snapshot-served view then matched
zero root functions (rootScopedView collapsed to whole-module, mappings
empty; only reproduced when another doc's analysis interleaved between two
view requests on a pool-size-1 backend, hence flaky under full-suite load).
Fix: OptimizedViewCache/IncrementalViewSnapshot carry a rootPathAliases set
(each analysis' temp path, accumulated across incremental rebuilds);
PrintModuleView's IsRootFile accepts any alias. Deterministic repro sequence
(rebuild in scratch/ if needed): open other doc, open view doc, ir O2,
edit+save the OTHER doc, asm O2, all on --lsp-pool-size 1.

RESOLVED (round 4): the "composition drift" theory was WRONG. Probes proved
first-analysis and re-analysis full builds are byte-identical (O0 and O2,
trivial doc and demo_view.cb modulo the intended edit). The DIVERGEs were the
incremental machinery itself, fixed by four changes:

- OptNone was silently ignored: optnone skipping lives in
  StandardInstrumentations (OptNoneInstrumentation), NOT the pass manager
  core. OptimizeViewModule built a bare PassBuilder, so every function pass
  re-optimized the "protected" kept bodies (jump-threading fgets etc.). Fix:
  register PassInstrumentationCallbacks + StandardInstrumentations. No-op for
  full builds (nothing carries optnone there).
- Even with optnone honored, module-level IPO (IPSCCP, GlobalOpt) still
  refines kept bodies. Fix: after the pipeline, parse the snapshot bitcode a
  second time and re-link it OverrideFromSrc over the kept functions (same
  promotion trick), so a kept function displays EXACTLY as the last full
  build produced it. Declaration attrs are recorded from work post-pipeline
  and reapplied after this link (linking unions them, and the snapshot's set
  is degraded - the bitcode round-trip re-canonicalizes intrinsic attrs,
  dropping e.g. mustprogress from llvm.va_start).
- Unchanged-global override remapped initializer struct types to arbitrary
  isomorphic ones (@__active_allocator: __iface_fat_ptr -> __closure_fat_ptr).
  Fix: for zero-initialized unchanged pairs, reduce src to a declaration so
  the work definition keeps its type names (zero content cannot hide a
  GlobalOpt-folded initializer, so this is safe; non-zero pairs still take
  the snapshot side).
- The link warned about mismatched datalayouts: the fresh clone had not been
  through OptimizeViewModule's triple/DL normalization yet. Aligned before
  linking.

Residual DIVERGEs after round 4 are three attr/metadata-level deltas, uniform
across all demo scenarios, none structural (function sets 24/24, all bodies
instruction-identical): (1) intrinsic-declaration attr comments differ by
mustprogress (creation-time attrs vs pipeline-created decls vs bitcode
round-trip canonicalization - the fast reopt=0 path returns the snapshot
verbatim and has no pipeline to re-infer); (2) a reopt'd body carries an
extra range() param attr (the incremental pipeline derives more facts from
already-optimized kept callees than the full pipeline has at the same point);
(3) one !inline_history metadata tag present in full but not inc (LLVM
inliner bookkeeping; inlining happened in both, via different step counts).
These are inherent to re-running a pipeline over a partially frozen module.
MAINTAINER RULING NEEDED: is instruction-identical + attr/metadata-delta an
acceptable bar, or must the driver normalize these classes too? The snapshot
struct stays file-serializable so the same machinery can later persist next
to -o outputs for incremental normal builds (maintainer request 2026-08-30).
CFLAT_VIEW_INC_DUMP=1 prints the reopt set per rebuild for diagnosis.

## Rejected: two-stage SLL parsing (investigated 2026-08-30)

Parse (~300 ms on test_basic) looked like the biggest single analyze cost, so the
standard ANTLR two-stage strategy (PredictionMode::SLL + BailErrorStrategy, LL
fallback on ParseCancellationException) was implemented and measured. Outcome: SLL
bails within ~5 ms on ordinary valid input - test_basic trips it at the close-paren
of a plain method declaration ('int get(int i)' line 32), and core files trip it
too - so every parse paid a wasted SLL attempt plus the full LL parse. The grammar
is not SLL-clean (likely the cast/paren-expression and declarator ambiguities), and
making it so is a grammar-redesign project, not a tuning change. Change reverted;
the Lex/Parse trace split that produced the finding was kept. If parse time ever
matters enough, the real project is an SLL-clean grammar refactor - measure with
the same probe (TimeTraceScope ParseSLL/ParseLLFallback + bail-token print).

## Constraints learned this round

- View emission must never mutate the analyzed module: PrintModuleView clones first
  (CloneModuleForView), which is what makes slot reuse safe. Keep that invariant.
- The core bitcode cache changes module composition (no debug info in cached core, so
  no core DILocations); performance work must be measured at a pinned cache state
  (--init-local after every build) or numbers are not comparable.
- Mapping faithfulness is checkable: retained functions must be instruction-identical
  to the whole-module print modulo metadata/local-label renumbering - see
  scratch/check_root_subset.py.
