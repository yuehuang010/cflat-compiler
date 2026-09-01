# Surfacing LLVM optimization information in the IDE

Status: Tiers 1, 2 and 3 DONE (2026-08-29) on branch `feature/optimization-info-ide`;
Tier 4 and the later phases PROPOSED. Verified on Windows: `cmake_build.bat release`,
`test.bat Release` (all passed, 0 skipped), `test_lsp.bat Release` (all passed, incl. the
`optimizationInfo: tier 1 facts` scenario extended to cover Tiers 2/3), `test_example.bat
Release` (100/0/41), extension `build.bat`.

The whole surface is one LSP request, `cflat/optimizationInfo`
(`{uri, optLevel?: 0|1|2 = 2, remarks?: bool = true}`), answering:

```
{ optLevel,
  functions: [{ name, symbol, startLine, endLine, irInstructions, machineInstructions,
                bytes, stackBytes, spills, reloads, eliminated, inlinedInto,
                lines: [{ srcLine, irInstructions, machineInstructions, inlined }] }],
  costs:          [{ kind, detail, srcLine, bytes, count }],          // Tier 2
  instantiations: [{ base, count, bytes, symbols }],                  // Tier 2
  remarks:        [{ pass, name, kind, message, function, file,       // Tier 3
                     srcLine, srcColumn, calleeName, calleeLine, args }],
  remarksTruncated }
```

Tier 1 deviations from the spec below, all deliberate:

- `estimatedCycles` was NOT implemented. It is a Tier 4 static estimate, and the standing
  "prefer decisions over predictions" rule says ship the facts first. Deferred to Phase 3.
- `bytes` WAS implemented, via `llvm::object::computeSymbolSizes` over an in-memory object
  emission. `getSymbolSize` is not usable: COFF symbols carry no size field.
- `stackBytes` / `spills` / `reloads` come from LLVM's own codegen remarks (`prologepilog`
  `StackSize` -> `NumStackBytes`; `regalloc` `SpillReloadCopies` -> `NumSpills`,
  `NumReloads`) captured by a scoped `DiagnosticHandler`, not from scraping asm text.
  The handler must override `isAnyRemarkEnabled()` - the emitters skip building a remark
  at all otherwise.
- Function matching disambiguates overloads by DISubprogram line and file, not by name
  alone. Name matching alone collapses `int scale(int)` and `float scale(float)` onto one
  entry.
- `lines` IS populated. It was specified as Phase 2, but the server side was finished ahead
  of the UI so tomorrow's VS Code work has the full data surface to design against. Shape:
  `{srcLine, irInstructions, machineInstructions, inlined}`, ascending, only lines that
  emitted something, each clamped to its function's source range.

  Two properties make it directly usable for gutter heat / inlay hints:
  - Attribution follows the innermost frame in the analyzed file, so an inlined callee's
    work stays on the callee's own line. A line inside an ELIMINATED function can still
    carry counts, flagged `inlined: true` - the code moved, it did not disappear.
  - A line absent from the array emitted nothing at that level. That is the "this line
    vanished" signal, and it is the difference between -O0 and -O2 worth rendering.

  Worked example (helper inlined into main at -O2): at -O0 every line is attributed; at -O2
  the helper's body lines report `inlined: true` while main's folded statements drop out of
  the array entirely.

  Not available per line: `bytes`. Byte sizes come from object symbol sizes, which are
  per-symbol, not per-line. Do not synthesize a per-line byte figure from instruction counts.

## Tier 2 and Tier 3 as built

Tier 2 (cflat-specific facts) is two arrays:

- `costs` - allocation and destruction the developer did not write literally. Each entry is
  `{kind, detail, srcLine, bytes, count}`, e.g. `{"alloc", "calloc", 10, 0, 1}`. `kind` is
  the category to render (`alloc`, `free`); `detail` is the callee. `bytes` is only non-zero
  when the size is a compile-time constant, so treat 0 as "unknown", never as "free".
- `instantiations` - generic monomorphization, grouped by template base, from
  `gts.instantiatedGenerics`. That registry is authoritative and is the reason this is Tier
  2 rather than a guess: an earlier attempt derived bases by parsing mangled symbol names
  and produced garbage (`_medianPivot_void_list`, `join_string_stringlist`).

  `bytes` is the code size charged to that base AT THE REQUESTED OPT LEVEL, which means it
  legitimately collapses under optimization: `list count=2 bytes=11754` at -O0 becomes
  `list count=2 bytes=0` at -O2 once every instantiation is inlined away. That is the same
  rule every other number in this response follows - it describes the module as built, not
  a hypothetical - and the count is what stays stable enough to render.

Tier 3 (`remarks`) is LLVM's own optimization remarks, captured by a scoped
`DiagnosticHandler` installed across BOTH the IR pipeline and codegen. `args` is the
remark's structured key/value payload, and it is the part worth rendering - an `inline`
`Inlined` remark carries `Callee`, `Caller`, `Cost` and `Threshold`, so "why did this not
inline" is answerable with numbers rather than prose. `remarksTruncated` reports the 400
-remark cap. `remarks: false` skips collection entirely and still returns Tier 1 and 2.

Three implementation facts worth not rediscovering:

- The handler must override `isAnyRemarkEnabled()` to return true, AND re-check the pass
  allowlist inside `handleDiagnostics`. Some emitters bypass the filtering `emit()`
  overload, so an allowlist consulted only at the enable check leaks `sdagisel` and
  `asm-printer` noise.
- The handler is restored by an RAII guard because the context destroys it on restore;
  `truncated` is therefore held by reference, not by value.
- Only functions in the request's own file are reported, and that is NOT decidable by
  filename - see below.

### cflat emits ONE DIFile for the whole module

This is a pre-existing compiler-wide limitation, discovered here and worked around here,
not fixed here. Imported core-library code carries the ROOT file's name together with its
own line numbers. A 9-line source therefore drew remarks at lines 44, 54, 270 and 383, all
claiming to be that file. File-based filtering cannot separate user code from core code,
and the existing IR/asm view has the same blind spot.

The workaround is a two-factor filter, and both factors are load-bearing:

1. `lineInUserRange(line)` - the line must fall inside one of the file's own function
   ranges, taken from `LspSymbolIndex::FunctionsIn(file)`.
2. `entryBySymbol.count(symbol)` - the enclosing symbol must be one of this file's own
   functions. Widened 2026-08-31 to `entryBySymbol || preoptEntryBySymbol`, so a caller the
   optimizer erased still counts as this file's code; it is the same set of user functions
   either way, just indexed before optimization as well as after.

Factor 1 alone admits core code whose line numbers happen to collide with a user function's
range. Factor 2 alone admits nothing useful for inlined code, where the enclosing symbol is
the user's function but the attribution must still be walked outward through the inline
chain to the first frame that is actually in range. Do not simplify either away. If debug
info ever gains per-import DIFiles, this whole filter collapses to a filename compare.

## Goal

Show the developer what the optimizer actually did to their code, in the editor, without
making them read IR. Today the only window into optimization is the "Show LLVM IR" /
"Show Assembly" view (`cflat/viewAssembly`), which is a full-file dump the user has to
read and interpret. The information a developer usually wants - did this inline, did this
survive, how big is this, where is the time going - is derivable from data the compiler
already produces, and can be answered in place.

Non-goal: becoming a profiler. Phases 1-3 are entirely static (compile-time); real
measured performance is Phase 4 and is deliberately kept behind a separate opt-in.

## What already exists (verified 2026-08-29)

- `LLVMBackend::LineMapping` (`cflat/LLVMBackend.h:531`) - `{srcLine, viewStart, viewEnd,
  stack}` where `stack` is the full inline attribution chain (`LineFrame{file, line, func,
  root}`). This is the source<->output correlation the whole feature rides on, and it is
  already accurate through inlining.
- `LLVMBackend::PrintModuleView` (`cflat/LLVMBackend_EmitAndLink.cpp:287`) - clones the
  module, optionally runs a private O2 pipeline, emits IR or asm text plus mappings.
  Materializes the core first (`MaterializeCoreIfLazy`) - load-bearing on a warm cache.
- `VectorizeDiagnosticHandler` (`cflat/LLVMBackend.cpp:3455`) - proves the LLVM
  optimization-remark channel is already wired into the backend and consumable. It is
  currently narrowed to `loop-vectorize` for the `vectorize` keyword's diagnostics.
- `LspSymbolIndex::FunctionRange` (`cflat/LspSymbolIndex.h:32`) - `{name, file, startLine,
  endLine}`, with `FunctionsEnclosing(file, line)`. This is what a CodeLens anchors to.
- `cflat/LspServer.cpp` `HandleViewAssembly` (:1345) / `RunAnalysisOnSlot` (:1603) - the
  request path, including `SetAnalyzeDebugInfo(true)`, which is what makes debug locations
  exist at all during LSP analysis. Any optimization-info request MUST set it too.
- Extension client: `vscode-extension/src/extension.ts` - `CflatViewContentProvider`,
  `TextEditorDecorationType` highlighting, the view QuickPick in `showCompilerView`.

The practical consequence: Phase 1 needs no new analysis infrastructure. It needs a new
request that runs the existing pipeline and counts.

## The data, in four tiers

Tiers are ordered by trust, and trust determines how loudly a datum may be displayed.
Facts are shown prominently; predictions are shown quietly and labelled.

### Tier 1 - counted from output we already generate

Cheapest and highest trust. All of it falls out of `LineMapping` plus the emitted text.

- Instruction count and byte size, per function and per source line.
- **"This line emitted nothing."** An empty mapping set means folded, DCE'd, or hoisted
  away. Developers are routinely wrong about this, and it costs nothing to compute - it is
  the same data the existing jump heuristic already consumes.
- Inline fan-out, from `LineFrame::root`: "inlined into 7 call sites" on the callee,
  "call inlined" on the caller.
- Stack frame size (prologue `sub rsp, N`).
- Spill/reload count.
- What survives on a line: calls remaining, branches, `memcpy` and its size, `malloc`/`free`.

### Tier 2 - cflat-specific, and the actual differentiator

Generic C/C++ tooling cannot show these, because they are facts about cflat's semantics.
This tier is what stops the feature from being a second-rate Compiler Explorer.

- **Ownership cost made visible**: "512-byte struct copy here", "destructor runs here",
  "string copied - `move` would elide this". The ownership model makes these questions
  constant and they are currently invisible without reading IR.
- **Monomorphization bloat**: `Box__int`, `Box__string`, ... - "14 instantiations, 8 KB
  total". Nothing surfaces this today, and generic bloat is silent by construction.
- **Devirtualization**: interface call that stayed an indirect vtable dispatch vs one that
  got resolved to a direct call.
- **Did `vectorize` deliver**: the pass/fail decision is already computed; rendering it as
  a check or cross on the loop is nearly free.

### Tier 3 - LLVM optimization remarks (the "why" channel)

Generalize `VectorizeDiagnosticHandler` into a full remark collector (either widen the
`DiagnosticHandler` categories or use `remarks::` serialization). High-value categories:

- `inline`: "not inlined into `foo` because too costly (**cost=350, threshold=225**)".
  The numeric distance from the threshold is the single most actionable number LLVM emits.
- `loop-unroll`: unrolled by 4 / not unrolled because trip count unknown.
- `gvn` / `licm`: load eliminated, N instructions hoisted, "load clobbered by store"
  (i.e. aliasing feedback).
- `sroa`: "alloca not promoted because address escapes" - reads as ownership feedback in
  cflat terms.
- `prologepilog`: spills, reloads, stack size - from the source rather than scraped out of
  the asm text.
- `size-info` / `annotation-remarks`: per-function instruction mix.

### Tier 4 - performance and cycles

Two genuinely different things. Conflating them is the main trap in this feature.

**Static (no run required):**

- `BlockFrequencyInfo` - relative execution frequency derived from branch probabilities.
  "This line runs ~1000x relative to function entry." This is the hot path *without a
  profiler*, and the analysis is already computed inside the pipeline.
- `TargetTransformInfo::getInstructionCost` - per-instruction reciprocal throughput.
  Nearly free, in-process.
- **`BFI x TTI cost`, summed per line, is the best derived metric available here**: a
  static "cycles attributable to this line" needing no harness and no user action.
- `llvm/MCA` as a library - per-basic-block estimated cycles, IPC, port pressure, and a
  named bottleneck ("limited by port 0" vs "limited by the dependency chain"). Surface on
  loops only. Caveat: MCA models scheduling, not cache - it will confidently mispredict
  any memory-bound loop. Never present its output as measurement.

**Dynamic (needs a run):**

- `--profile-generate` / `--profile-use`: real per-line counts, and the same data doubles
  as genuine PGO input. Larger project; the IDE payoff is a true heat map.
- Cheaper alternative: sample with ETW (Windows) or `perf` (Linux) and map addresses back
  through the DWARF already emitted under `-g`. No instrumented rebuild, gives real cycles
  and cache misses, but needs a run harness.

## Surfacing: the mechanisms, and why

| Surface | Best for | Cost |
|---------|----------|------|
| **CodeLens above a function** | Per-function summary; clickable into the IR view | Low |
| Decoration `after: {contentText}` | Right-of-line text; same mechanism the extension already uses | Low |
| Inlay hints | Polished per-line facts, with tooltip + command | Medium |
| Gutter + overview ruler heat | Ambient hotspot map; the ruler shows the WHOLE file at once | Medium |
| Hover | Detail layer under everything else; zero noise, on demand | Low |
| Problems panel (Hint severity, separate collection) | Actionable misses only; filterable/searchable | Low |
| Tree view ("Optimization Explorer") | "Where is my binary going" - a whole-program question annotations cannot answer | High |

Target layering once mature: **gutter heat (ambient) + CodeLens (summary) + hover (detail)
+ opt-in Problems hints (actionable)**, with inlay hints reserved for a small curated set
of per-line facts. Everything behind one toggle so the default editing experience is
unchanged.

Per-line annotation must be filtered aggressively. A number on every line is unreadable;
gate on hot, large, or carrying-a-missed-remark.

## Phase 1 - per-function CodeLens (the agreed start)

One CodeLens above each function definition:

```
142 instrs - 96 B - 3 spills - inlined 7x - est ~210 cy
int helper(int value) { ... }
```

Clicking it opens the existing IR view already jumped to that function, which composes
directly with the jump work landed in `ir-asm-view-inline-attribution.md`.

Scope for Phase 1: **Tier 1 only**, plus the estimated-cycles field if `BFI x TTI` proves
cheap to wire; otherwise ship without it and add in Phase 3. No remarks, no per-line
annotation, no heat map.

### Protocol

New request `cflat/optimizationInfo`:

```
params:  { uri: string, optLevel?: 0 | 1 | 2 }        // default 2
result:  { optLevel: number, functions: FunctionInfo[] }

FunctionInfo {
    name: string
    startLine: number       // root-file line, from LspSymbolIndex::FunctionRange
    endLine: number
    instructions: number    // machine instructions in the emitted function
    bytes: number           // emitted size
    stackBytes: number      // frame size, 0 if leaf/none
    spills: number
    reloads: number
    inlinedInto: number     // distinct call sites the body landed in
    eliminated: boolean     // no code emitted at all
    estimatedCycles?: number    // Tier 4 static estimate; OMITTED if not computed
    lines?: LineInfo[]      // reserved for Phase 2; not populated in Phase 1
}
```

`lines` is declared but unpopulated in Phase 1 so Phase 2 is an additive change. Both
sides of this protocol live in this repo, so the compatibility burden is one release, but
there is no reason to churn the shape twice.

### Server

- `HandleOptimizationInfo` alongside `HandleViewAssembly` in `cflat/LspServer.cpp`,
  enqueuing on the same analysis slot machinery. It MUST set `SetAnalyzeDebugInfo(true)` -
  without it there are no debug locations and every mapping is empty.
- New backend entry point, e.g. `LLVMBackend::CollectOptimizationInfo(int optLevel,
  std::vector<FunctionOptInfo>& out)`, factored to share the clone +
  materialize + optimize prologue with `PrintModuleView` rather than duplicating it. That
  prologue is where the warm-cache materialization bug lived; it must not be copy-pasted.
- Function ranges come from `LspSymbolIndex::FunctionRange`; per-function counts come from
  the emitted MC/asm correlated through `LineMapping`. Attribute by the mapping's ROOT
  frame so an inlined callee's instructions are counted against the caller that absorbed
  them, and reported separately as `inlinedInto` on the callee.

### Client

- `vscode.languages.registerCodeLensProvider({scheme: 'file', language: 'cflat'}, ...)` in
  `extension.ts`, next to the existing registrations (~:379).
- Debounced, on-demand refresh - see Cost below. Fire on save and on explicit command, not
  on every keystroke.
- Setting `cflat.optimizationInfo.enable` (default **false** for the first release), plus
  `cflat.optimizationInfo.optLevel` (default 2).
- Command `cflat.showOptimizationInfo` to force a refresh, and the CodeLens command target
  reusing the existing view-open path with the function's start line.

### Cost - the one real design constraint

This requires running the -O2 pipeline, which normal `--check` analysis does not do. It
cannot ride the per-keystroke analysis path. It must be explicitly requested and debounced,
and the result cached per (uri, optLevel, document version). On a large file the O2
pipeline is not free, and a CodeLens provider that triggers it on every edit would make the
editor unusable.

### Acceptance

- CodeLens appears above every function in a `.cb` file with the toggle on, and nowhere
  with it off.
- Numbers agree with the IR/asm view for the same function at the same opt level. A
  function the optimizer removed reports `eliminated` rather than zeros.
- Clicking the lens opens the view scrolled to that function.
- Editing does not trigger a pipeline run; save or explicit command does.
- `test_lsp.bat` gains scenario coverage for `cflat/optimizationInfo` (extend the existing
  fixture file - do not add a new test file).
- `test.bat` Release and `test_lsp.bat` Release green.

## Later phases (recorded, not scheduled)

- **Phase 2 - per-line data.** Server side DONE (see above). INLINE MARKERS DONE
  (2026-08-31, see below). Remaining client work: render `lines` as heat (gutter /
  overview ruler), filtered to lines that are hot, large, or eliminated.

## Inline markers in the editor (DONE 2026-08-31)

The first visualization of the optimization data. A dim `\u21A6` sits after each call the
optimizer inlined, and after the definition of each function that was inlined somewhere;
hovering gives the callee, the decision and LLVM's cost/threshold numbers. Setting
`cflat.optimizationInfo.inlineAnnotations` (default true, gated behind
`optimizationInfo.enable`); repaints on the CodeLens cache signal, so one -O2 run feeds
both surfaces.

Four decisions worth not relitigating:

- **The glyph is call-sites-only.** The definition line already carries a CodeLens saying
  the same thing; a glyph beside it repeated one fact twice. The per-function inline story
  moved into that lens instead - see the hover note below.
- **Only inlining is marked.** A call the optimizer left alone gets NO glyph. An
  annotation on every call is wallpaper; the value is in the lines that are marked, and
  an unmarked line reading as "nothing happened here" is the whole point. The
  not-inlined remarks are still shown, but only inside the hover of a function that WAS
  inlined somewhere ("inlined at 1 of 2 call sites"), where they carry the actionable
  cost-vs-threshold gap.
- **Glyph choice is constrained, not cosmetic.** A solid triangle reads as a run/debug
  button and an arrowhead reads as navigation; both were rejected on sight. Colored or
  emoji glyphs compete with diagnostics for attention. `\u21A6` in
  `editorCodeLens.foreground` is the result. It is one constant, `INLINE_GLYPH`.
- **The server resolves the mangled callee, not the client.** `args.Callee` is a mangled
  symbol (`_small_helper_i32_i32_`). `OptRemark` therefore carries `calleeName` and
  `calleeLine`, resolved against a NEW pre-optimization symbol index
  (`preoptEntryBySymbol`) - the view-side index cannot name a function the optimizer
  erased, which is exactly the function an "inlined away" marker is about. The same index
  now also fills `symbol` for eliminated functions, and widens the remark caller filter so
  decisions made inside a caller that itself got inlined are no longer dropped.
- **`inlinedInto` is NOT the "was this inlined" signal.** It counts functions that still
  hold absorbed code, so a helper that was inlined and then constant-folded reports
  `inlinedInto: 0` while the remark correctly says `Inlined`. The remarks are
  authoritative for the decision; `inlinedInto` is authoritative for surviving code.

Known limitation: LLVM reports `srcColumn` as 0 for inline remarks, so a marker anchors to
the END of the call-site line and several calls on one line share one marker (the hover
lists them all). Nothing in the response supports finer placement today.

`bytes` is rendered as "Function size", not "emitted size": it IS the function's
machine-code size, taken from its symbol in an in-memory object emission. One caveat the
label hides - for a generic it is the SUM over every instantiation, because
`entryBySymbol` maps every mangled instantiation back to the one source function and the
byte walk uses `+=`. The Tier 2 `instantiations` array is the surface that breaks that
down properly.

Neither the IR instruction count nor the mangled symbol is EVER shown to the user, in
the lens or the hover. It is a
fact about a representation they never see, and it invites comparisons against the machine
code that do not hold. Emitted BYTES leads instead; machine instructions follow when
available. `irInstructions` stays on the wire and in the client interface - the server
still reports it - it is simply not rendered. Because the IR count used to be the one
figure guaranteed to exist, the lens now needs an explicit "no size information" fallback
for the case where every counter is unavailable at once. The symbol went for the same
reason plus one of its own: `symbol` holds only the FIRST matching definition while
`bytes` sums them all, so on a generic it names one instantiation beside a size covering
every one of them.

The hover does not list call sites AT ALL. It shows one row per DISTINCT decision, and a
`[Show N call sites]` link hands the locations to VS Code's own References peek
(`editor.action.showReferences`) - a navigable, previewable list is that widget's job, and
a hover is the wrong container for one of unbounded length. LLVM reports every inline
remark at column 0, so `callRange` recovers the column by searching the line text for the
callee name, falling back to the whole line; without that the peek highlights column 0 on
every hit.

Rows carry a site count only when the outcomes DIFFER - a single outcome covering
everything needs none, because the summary line above already gave the total. Measured before
building it: 10 call sites spread over plain, if-guarded and loop-nested calls all reported
the identical `(TooCostly, cost 595, threshold 337)` - cost is mostly a property of the
CALLEE, so a function called from 200 places collapses to a handful of rows, usually one.
The grouping key is the rendered description string itself, so there is no parallel key to
drift from the text.

`remarksTruncated` is now plumbed to the client for this: past the server's 400-remark cap
the per-function counts are lower bounds, and "inlined at 3 of 5 call sites" would be a
lie. The hover says so instead.

Clicking the CodeLens opens a HOVER, not the IR view. A CodeLens `tooltip` is a plain
string - no markdown, no links - so the lens command
(`cflat.showOptimizationDetail`) puts the cursor on the function and runs
`editor.action.showHover`; a `HoverProvider` then returns `buildFunctionDetail` as a
MarkdownString carrying a `[Show IR](command:cflat.showIrForFunction?...)` link. Command
links require `markdown.isTrusted = true`; without it VS Code renders the link dead, with
no error. Jumping straight to the IR view on click was the old behaviour and was wrong:
the numbers are the answer most of the time, and the IR is the follow-up.

Client layout: the pure aggregation lives in `vscode-extension/src/optimization_info.ts`
with no `vscode` import, so it is unit-testable directly (`buildInlineAnnotations` cases in
`src/lsp_integration.test.ts`); `extension.ts` keeps only the decoration painting. Do not
move it back - importing `extension.ts` from vitest drags in `vscode-languageclient`, whose
CommonJS `require('vscode')` no alias can intercept.
- **Phase 3 - remarks (Tier 3) and Tier 2 cflat facts.** Widen the diagnostic handler;
  surface actionable misses as Hint diagnostics in a dedicated collection, with the full
  remark text on hover.
- **Phase 4 - measured performance.** `--profile-generate`/`--profile-use` or an
  ETW/`perf` sampling path mapped back through DWARF. Separate opt-in; must never be
  confused with the static estimates from Phase 1/3.

## Standing decisions

- **Prefer decisions over predictions.** "Inlined", "unrolled by 4", "96 bytes" are facts
  and build trust. "~210 cycles" is a guess and erodes trust the first time it is wrong.
  Facts prominent, estimates quiet and explicitly marked as estimates.
- **Match the real build.** If the annotation says -O2 and the project builds -O0, the
  user chases ghosts. Either follow the project's actual build configuration or label the
  mode unmissably in the UI.
- **Default off.** The feature is opt-in until it has proven it does not degrade editing
  responsiveness.
