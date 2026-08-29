# IR/ASM view: full inline-stack attribution + selectable opt level

Status: DONE (2026-08-28) - implemented (server + extension + LSP scenario test), verified
on Windows: cmake_build.bat release, extension build, test.bat Release, test_lsp.bat Release
(50 scenario checks + 232-file sweep) all green. Note: `.cv_loc`/`.cv_file` (CodeView) parsing
was added alongside `.loc` since the Windows target emits CodeView directives.

## Goal

Improve the existing VS Code "Show LLVM IR" / "Show Assembly" feature (custom LSP request
`cflat/viewAssembly`) so that source<->view line mapping stays accurate through optimization,
especially across inlining:

1. Every mapping carries the FULL inline stack (callee line + each inlinedAt caller frame,
   with function names and files), not just the innermost root-file line.
2. Clicking a call-site line in the source highlights the asm/IR regions where the inlined
   callee body landed; clicking an asm/IR line highlights BOTH the callee line and the
   call-site line(s) in the source, and surfaces a readable description like
   "helper.cb:12 (helper) inlined into main at main.cb:40".
3. The optimization level is user-selectable (O0/O1/O2), default O2, instead of the
   hardcoded optimized=true -> O2.
4. The view's optimization pipeline matches the real build pipeline.

## Current state (all verified 2026-08-28)

- Client: `vscode-extension/src/extension.ts` - `CflatViewContentProvider` (virtual docs on
  scheme `cflat-view:`), request at :110, bidirectional highlight in `updateSelection`
  :144-177 driven by `mappings: {srcLine, start, end}` (1-based, root source file only).
  Scope QuickPick in `showCompilerView` :456-495 (whole file / current function, x optimized).
- Server: `cflat/LspServer.cpp` - `HandleViewAssembly` :1345-1423 parses
  `{uri, kind: "ir"|"asm", optimized?, filter?: {function?, line?}}`, enqueues an
  `IrRequest`; `RunAnalysisOnSlot` :1586 runs a full analysis with
  `SetAnalyzeDebugInfo(true)` (:1656 - load-bearing: debug locations only exist in LSP
  analysis when an IR request is attached), resolves line->function via
  `LspSymbolIndex::FunctionsEnclosing` (:1773-1787), calls `PrintModuleView` (:1791),
  serializes in `SendViewResult` :1818-1834.
- Backend: `cflat/LLVMBackend_EmitAndLink.cpp` `PrintModuleView` :217-623.
  - Clones the module (:225), optionally runs a private O2 pipeline (:324-340).
  - IR mappings: second clone, erase debug intrinsics (:394-401 - DEAD on LLVM 23, see
    Fix 4), print with `LineMappingAnnotationWriter` (:56-82) which records
    `{sourceLine(dbgloc), viewLine, viewLine}`; display copy is `StripDebugInfo`'d and
    printed separately; `ConsolidateLineMappings` :84-97 merges adjacent runs.
  - `sourceLine` lambda :370-375 walks `getInlinedAt()` and returns the INNERMOST
    root-file line only - this is the single-line collapse we are replacing.
  - Asm mappings: `addPassesToEmitFile(AssemblyFile)` (:439), then TEXT-parses `.file`
    (:463) / `.loc` (:478) directives, keeps only root-file ids (:494-520),
    builds runs in `appendAsmMappings` :522-559. `.loc` has no inline chain.
  - `LineMapping` struct: `cflat/LLVMBackend.h:523-528` `{int srcLine, viewStart, viewEnd}`.
- Real-build pipeline for comparison: `OptimizeModule` `cflat/LLVMBackend.cpp:3596-3830` -
  uses `MakeStdioSafeTLII` (registered BEFORE registerFunctionAnalyses, :3769-3771) and
  `PipelineTuningOptions` (loop + SLP vectorization on), `buildPerModuleDefaultPipeline`.
- Known frontend caveat (out of scope, do not try to fix): return-block functions
  (`return { ... }`) are inlined at the AST level and carry the call site's DILocation with
  no inline chain; they cannot be attributed to the callee. Same for anything the frontend
  expands inline. Document as a limitation in the code comment, nothing more.

## Protocol changes (custom LSP - both sides in this repo, no compat burden beyond one release)

Request `cflat/viewAssembly` gains:
- `optLevel?: 0 | 1 | 2` - optimization level for the cloned view module. Precedence:
  explicit `optLevel` wins; else legacy `optimized: true` means 2, `optimized: false`/absent
  means 0. Validate like the other params (-32602 on wrong type / out of range).

Response mapping entries become:
```
{ srcLine: int,            // innermost root-file frame line (unchanged meaning; >0)
  start: int, end: int,    // view line range, 1-based inclusive (unchanged)
  stack?: [                // present when depth > 1 OR any frame is non-root;
                           // innermost frame first (index 0 = where the code was written)
    { file: string,        // filename only (basename), for display
      line: int,
      func: string,        // display function name from DISubprogram::getName()
      root: bool }         // true if this frame is in the analyzed root file
  ] }
```
Omit `stack` entirely for the common non-inlined root-file case to keep payloads small.
Entries whose stack contains NO root-file frame are omitted (nothing to highlight), as today.
NEW: entries where the root-file frame is an OUTER frame (call site) but the innermost frame
is in another file (e.g. an inlined core-library function) ARE now emitted, with
`srcLine` = the outermost root-file frame's line - this is what makes call-site
highlighting work for cross-file inlining. Keep the rule simple: `srcLine` = the line of the
innermost frame that has `root == true`.

## Server work

### 1. `LineMapping` grows an inline stack
In `cflat/LLVMBackend.h` extend `LineMapping`:
```cpp
struct LineFrame { std::string file; int line = 0; std::string func; bool root = false; };
struct LineMapping { int srcLine = 0; int viewStart = 0; int viewEnd = 0;
                     std::vector<LineFrame> stack; };  // empty = plain root-file mapping
```
Build the stack from a `DILocation` by walking `getInlinedAt()`; for each frame take
`getFile()` basename, `getLine()`, `getScope()->getSubprogram()->getName()` (guard nulls),
and `root = isRootFile(frame file)`. Factor this into one helper used by both IR and asm
paths, replacing the `sourceLine` lambda (keep a thin wrapper that returns the innermost
root frame's line for `srcLine`). Only materialize/keep the stack when depth > 1 or a
non-root frame exists.

`ConsolidateLineMappings` (:84-97) must only merge adjacent runs whose srcLine AND stack are
equal (element-wise), otherwise distinct inline provenance gets smeared together.

### 2. IR path
`LineMappingAnnotationWriter::emitInstructionAnnot` records the full stack per instruction
via the shared helper. Emit a mapping when the stack contains ANY root-file frame (today:
only when the innermost root hit exists - same condition, but srcLine selection per the
protocol rule above).

### 3. Asm path - recover inline stacks despite `.loc` having none
`.loc <fileid> <line> <col>` identifies the INNERMOST frame only. Recover the stack by
correlation against the post-optimization IR module (the same `view` module that codegen
consumed):
- After the O-level pipeline runs and BEFORE `addPassesToEmitFile`, build a lookup:
  for each defined function F in `view`, for each instruction debug location L
  (`inst.getDebugLoc()`), key `(F name, innermost file path, innermost line, innermost col)`
  -> set of distinct full inline stacks seen.
- Extend `parseLocDirective` to also read the column (third integer; default 0 if absent).
- While scanning the asm text, track the current function: a line ending in `:` at column 0
  whose label matches a defined function symbol in `view` (handle the Darwin leading `_`
  like the existing per-function slicing at :568-620 does) sets the current function;
  keep the existing "any label finishes the open run" behavior for run boundaries.
- On each `.loc`, resolve the file id via the already-parsed `.file` table (keep ALL ids
  now, not just root ids - store id -> full path), look up
  `(current function, path, line, col)`:
  - exactly one stack -> attach it;
  - multiple stacks -> attach the longest common SUFFIX (outermost frames shared); if even
    the innermost frames disagree, fall back to a depth-1 stack of just the .loc frame;
  - no hit (e.g. codegen-synthesized location) -> depth-1 stack of the .loc frame.
- Emit the mapping iff the resulting stack has a root-file frame (srcLine rule as above).
This removes the current `rootAsmFileIds` filtering; delete what becomes dead.

### 4. LLVM 23 debug-record fix (mapping alignment)
The strip loop at :394-401 targets `DbgInfoIntrinsic`, which no longer exists as
instructions on LLVM 23 - debug info prints as `#dbg_declare`/`#dbg_value` RECORD lines
attached to instructions, which desynchronizes the mapping print (records present) from the
display print (StripDebugInfo'd). Replace the loop: for every instruction in the mapping
clone call `inst.dropDbgRecords()` (and delete the dead intrinsic-erase loop). Then VERIFY
alignment: in the new LSP test (below), assert that for some known mapping the view text at
`mappings[i].start` is an actual instruction attributable to that source line (e.g. the
`ret` of a known function), not off-by-N. If other line-count divergences remain between the
two prints (e.g. trailing metadata is fine - it is after all functions), fix them; if the
two-print design cannot be made to align exactly, print ONCE (the mapping clone, records
dropped) and use that same text as the display text - identical-by-construction beats
clever.

### 5. Unified, selectable pipeline
- Plumb `optLevel` (int) through `HandleViewAssembly` -> `IrRequest` -> `PrintModuleView`
  (replace the `bool optimized` parameter; update the `--out-asm` caller at
  `LLVMBackend.cpp:2429-2442` which passes optimized=false -> optLevel 0).
- In `PrintModuleView`'s pipeline block (:324-340): use the SAME construction as
  `OptimizeModule` (:3748-3785): `MakeStdioSafeTLII`-based TLI registered before
  `registerFunctionAnalyses`, `PipelineTuningOptions` with loop + SLP vectorization,
  `buildPerModuleDefaultPipeline(O1|O2)` chosen from optLevel. Factor the shared setup into
  a small helper if clean; do not fork the logic a third time.
- `CodeGenLevelFor(optLevel)` already exists (:182) - pass optLevel into
  `CreateOptTargetMachine` usage accordingly (it currently reads `cOptLevel_`; the view
  should use the requested level, not the CLI one - add a parameter with a default that
  preserves existing call sites).
- Update the "optimized away at O2" banner text (:278-312) to name the actual level.

### 6. Serialization
`SendViewResult` (:1818-1834): add `stack` array per the protocol when non-empty.

## Client work (`vscode-extension/src/extension.ts` + `package.json`)

1. `ViewMapping` gains `stack?: {file: string; line: number; func: string; root: boolean}[]`;
   validate it defensively like the existing fields (drop malformed entries' stacks, keep
   the mapping).
2. Opt level selection: after the existing scope QuickPick, when the chosen scope is an
   "optimized" one, show a second QuickPick O2 (default, preselected) / O1 / O0; remember
   the last choice like `lastViewChoiceId`. Send `optLevel` in the request (keep sending
   `optimized` too - harmless). Non-optimized scopes send optLevel 0.
3. `updateSelection` view->source: highlight EVERY root frame's line in the source (the
   innermost root frame with the existing decoration; add a second, dimmer decoration type
   for outer call-site frames), reveal the innermost one. Show a status bar message
   (auto-dismiss ~5s) rendering the stack innermost-first:
   `helper.cb:12 (helper) <- inlined at main.cb:40 (main)`. No message for depth-1 stacks.
4. `updateSelection` source->view: a view region matches the cursor line if `srcLine`
   matches OR any root frame in `stack` has that line - so clicking a call site lights up
   the inlined body regions.
5. Keep everything else (virtual doc scheme, refresh-on-save) unchanged.

## Tests (extend existing files ONLY - do not create new test scripts)

- Extend `vscode-extension/test/lsp_fixture_test.py` (and its fixture dir
  `cflat/test_lsp/fixtures/` - a new FIXTURE .cb file there is fine, that is where fixtures
  live) with a `viewAssembly` scenario: a small program where a helper function with a
  distinctive body is called from main and reliably inlined at O2 (make helper small; if
  needed mark nothing - default O2 inlining handles a tiny callee).
  Assertions, for both `kind: "ir"` and `kind: "asm"` with `optLevel: 2`:
  - response has non-empty `text` and `mappings`;
  - at least one mapping has `stack` with length >= 2;
  - that stack's innermost frame line == the helper-body source line, and some outer frame
    line == the call-site line, both `root: true`;
  - alignment spot-check per server Fix 4;
  - `optLevel: 0` on the same file returns mappings with no multi-frame stacks for the
    helper body inside main (helper not inlined).
  - legacy request WITHOUT optLevel but `optimized: true` still works (returns 200-style
    result, non-empty text).
- Run `test_lsp.bat Release` and `test.bat Release` - both must pass.

## Constraints (repo rules)

- ASCII only in code/comments/messages. Inline comments <= 2 lines.
- Error reporting via LogError/LogErrorContext only; no std::cout, no LogWarning.
- Do NOT touch `cflat/locales/` or root `vcpkg.json`.
- Do NOT git commit (stash allowed). Do not create new test entry-point scripts or
  `Test/test_*.cb` files; fixtures under `cflat/test_lsp/fixtures/` are allowed.
- Build with `cmake_build.bat release` (writes x64/Release/cflat.exe).
- Keep the extension TypeScript in the existing style; build it with
  `vscode-extension/build.bat` to type-check.

## Acceptance

1. `cmake_build.bat release` clean.
2. `test.bat Release` and `test_lsp.bat Release` fully pass, including the new
   viewAssembly scenario.
3. Manual protocol sanity (the python harness or a direct JSON-RPC exercise is fine):
   O2 asm view of a file with an inlined helper produces >= 1 mapping whose stack names
   the helper and the call site.
