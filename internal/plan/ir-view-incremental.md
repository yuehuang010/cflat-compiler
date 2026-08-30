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
