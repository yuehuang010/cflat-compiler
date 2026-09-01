# Inline cost breakdown for the IDE optimization view

Status: PROPOSED (2026-08-31). Child of `optimization-info-ide.md` (Tier 3 extension).

## Problem

The inline remarks collected by `RemarkCollector` (`LLVMBackend_EmitAndLink.cpp`) carry
only `Cost` and `Threshold` - that is all LLVM puts in the remark. The extension can say
"not inlined - too costly (cost 320 vs threshold 250)" but not WHY the cost is 320.
`optimization_info.ts` currently hides the numbers entirely as unitless internals; a
breakdown is what makes them actionable.

## Findings (verified against pinned LLVM 23.1.0 headers, 2026-08-31)

- `llvm/Analysis/InlineCost.h` exposes `getInliningCostFeatures(CallBase&, TTI&,
  GetAssumptionCache, ...)` -> `InlineCostFeatures`, an array of per-summand components
  of the heuristic inline cost. Public API, no LLVM patch.
- Feature list (`llvm/Analysis/InlineModelFeatureMaps.h`): additive summands
  (`call_penalty`, `call_argument_setup`, `indirect_call_penalty`, `switch_penalty`,
  `case_cluster_penalty`, `jump_table_penalty`, `switch_default_dest_penalty`,
  `unsimplified_common_instructions`, `callsite_cost`, `cold_cc_penalty`,
  `load_elimination`, `load_relative_intrinsic`, `lowered_call_arg_setup`,
  `nested_inline_cost_estimate`), credits (`sroa_savings`,
  `last_call_to_static_bonus`), and context counts (`constant_args`, `num_loops`,
  `dead_blocks`, `simplified_instructions`, `is_multiple_blocks`, `nested_inlines`,
  `threshold`). The extractor mirrors the same CallAnalyzer the default heuristic runs.
- The breakdown must be computed on the call site AS IT EXISTS MID-PIPELINE (after
  earlier inlining/simplification) or it will not reconcile with the remark's Cost.
  LLVM 23 has a first-class hook: `PluginInlineAdvisorAnalysis`
  (`llvm/Analysis/InlineAdvisor.h`). Register it on the ModuleAnalysisManager before the
  pipeline runs; `InlineAdvisorAnalysis::Result::tryCreate` prefers it automatically.
  cflat owns the pipeline setup (`RunViewPipeline`, `LLVMBackend_EmitAndLink.cpp`
  ~524-541), so this is a direct `moduleAnalysis.registerPass` - no dynamic plugin lib.
- Rejected alternatives:
  - Post-hoc `getInliningCostFeatures` at LSP query time on the pre-inline module:
    cheap and on-demand, but module state differs from decision-time state, numbers
    would not match the remark. Misleading; do not build.
  - `InlineCostAnnotationPrinterPass` (`print<inline-cost>`): per-instruction cost
    comments inside the callee. Text-stream output, needs parsing. Possible later
    drill-down ("which callee lines cost the most"), not this plan.

## Design

### Server

1. `BreakdownInlineAdvisor : llvm::InlineAdvisor`, wrapping an owned
   `DefaultInlineAdvisor`. `getAdviceImpl(CallBase& cb)`:
   - delegate the decision to the default advisor (behavior byte-identical);
   - apply the same root-file location filter the RemarkCollector uses; skip non-root;
   - call `getInliningCostFeatures(cb, calleeTTI, getAC, ...)` with analyses pulled from
     the FAM the factory received; record `{callerName, calleeName, file, line, col,
     features}` into a sink.
2. `PluginInlineAdvisorAnalysis::AdvisorFactory` is a PLAIN function pointer - no
   captures. Route the sink through a `thread_local` pointer set for the duration of
   `RunViewPipeline` (LSP analysis pool runs pipelines on multiple threads; a plain
   static races). RAII guard to clear it.
3. Register only when remark collection is active (the optimizationInfo path). The
   normal compile path stays untouched - zero cost there.
4. Merge: after the pipeline, match records to collected inline remarks by (callee name,
   file, line, col) and append nonzero features to that remark's `args`
   (`sroa_savings=12`, `call_penalty=50`, ...). No protocol change: `OptRemark.args`
   already flows end-to-end. Unmatched records are dropped; remarks with no matching
   record keep their current shape (additive change).

### Client (`optimization_info.ts`)

On the inline decision detail, render top contributors, largest first, credits signed:
"cost 320 vs threshold 250: instructions 240, call penalty 50, switch 40, SROA savings
-10". Show only nonzero summands; cap the list (say 6) with a "+n more". Context counts
(constant_args, num_loops...) go in a tooltip line, not the sum.

## Constraints and caveats

- Feature extraction re-runs the full call analyzer per call site: roughly 2x inliner
  analysis time. Acceptable because it only runs on the IDE view clone, and only when
  remarks were requested. Measure on the large-file LSP fixture before accepting.
- Do NOT promise `sum(features) == Cost` in the UI. Summands + credits approximate the
  final cost; boundary cases (never-inline, always-inline, cost-benefit path) diverge.
  Present as "major contributors".
- The advisor must not change decisions. Acceptance includes byte-identical IR for the
  view pipeline with and without the advisor registered (existing incremental-view
  byte-exactness test is the harness for this).
- `kMaxCollectedRemarksPerPass` clamps remarks; the record sink needs the same cap or a
  large source leaks memory in the sink for remarks that were never collected.

## Steps

1. Server: advisor + registration + merge into remark args. Extend the existing
   `optimizationInfo` LSP fixture scenario to assert a known call site carries at least
   one breakdown arg at -O2. No new test files.
2. Extension: render contributors in the inline decision UI; `build.bat` +
   `test_lsp.bat` + extension test suite.
3. (Recorded, not scheduled) callee line-level drill-down via
   `InlineCostAnnotationPrinterPass` if the summand view proves insufficient.

## Verification

`cmake_build.bat release`, `test.bat Release`, `test_lsp.bat Release`, extension
`build.bat` + `vscode-extension/test/lsp_fixture_test.py`. Confirm view-pipeline output
unchanged (incremental byte-exactness suite) and measure optimizationInfo latency delta
on the largest fixture.
