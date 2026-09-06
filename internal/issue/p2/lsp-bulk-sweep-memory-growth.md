# LSP bulk sweep grows to ~3 GB: three unbounded process-lifetime caches

## Summary

`test_lsp.bat`'s bulk source sweep (`vscode-extension/test/lsp_bulk_test.py`, 238 files,
`--lsp-pool-size 4`) drives one long-lived `cflat.exe` from ~250 MB to ~3.0 GB working set,
monotonically, with no plateau.

It is NOT a classic leak - `ResetForReanalysis` releases per-analysis state correctly. It is
three caches that are retained for the process lifetime BY DESIGN and have no eviction policy
and no size bound. Measured 2026-09-05, Windows, Release.

## Repro (no LSP needed)

The CLI `--check` batch path reuses one backend across files through the same
`ResetForReanalysis`, and reproduces it:

```bash
x64\Release\cflat.exe --check <43 distinct Test\test_*.cb>    # 52 MB -> 619 MB WS
```

LSP sweep for reference: `--lsp-pool-size 4` -> 3.01 GB; `--lsp-pool-size 1` -> 1.63 GB.
The pool multiplies contributor 2 below (per-backend) but not 1 (process-static).

## Root causes, with measured attribution

Live bytes were measured with `mi_heap_visit_blocks(mi_heap_main(), true, ...)` (mimalloc
backs `operator new` via `main.cpp`; `mi_stats_get`'s `malloc_requested` reads 0 in this
build, so block-walking is the only way to get live bytes). Live grew 58 MB -> 409 MB /
760k -> 3.55M live blocks over the 43 files, sampled after each reset. Per-file deltas:

| file | +live MB | +parseTrees | +cFileSigRows |
|------|---------|-------------|----------------|
| test_windows.cb | 105 | 2 | 54269 |
| test_hpc_kernels.cb | 58 | 24 | 0 |
| test_windows_cache.cb | 39 | 0 | 54191 |
| test_collection_leaks.cb | 36 | 8 | 3546 |
| test_math.cb | 21 | 8 | 0 |

**1. `cFileSigCache_` (`LLVMBackend.h`) - largest single contributor.**
`static inline std::unordered_map<std::string, CFileSigCacheEntry>`: process-wide (shared by
every LSP backend slot), never evicted, no bound. One `windows.h` binding is ~54k rows
(sigs + enums + records + macros + funcMacros + globals + recordAliases + typeAliases + deps)
and ~105 MB live. The bulk sweep binds windows.h from `Test/test_windows*.cb`,
`example/windows/*`, `example/COM/*`, `core/ui_native/win32.cb`, `core/ui_canvas/win32.cb`
and more.

Sub-defect: `test_windows_cache.cb` adds a SECOND ~54k-row copy of the same header
(+39 MB). The key is `"<canonical .h>|<include dirs>"`, so the same header reached under a
different include-dir set (or via the `cache` clause) is stored again rather than shared.

**2. `parseTreeCache_` (`LLVMBackend.cpp` `GetOrParseFile`).**
Every imported ANTLR parse tree - input stream + lexer + token stream + parser + tree - is
retained for the process lifetime, mtime/size-validated but never evicted. This is the whole
of `test_hpc_kernels.cb`'s +58 MB (+24 trees, 0 sig rows). Entries grew 31 -> 102 over just
43 files; the full 238-file sweep caches the union of every import in the tree. It is
per-backend, so a pool of N multiplies it.

Note `CLAUDE.md` describes this cache as covering "implicit core-library imports only
(runtimeDir/core)". That is inaccurate - `GetOrParseFile` is the only parse path for ALL
imports, core and user alike. Purging only the non-core entries on reset recovers 23 MB of
619; most of the retained weight is core trees, which are retained deliberately.

**3. ANTLR's process-global prediction caches.**
The generated parser holds `cflatParserStaticData->decisionToDFA` and `->sharedContextCache`
as file-scope statics (`build/*/antlr_generated/CFlatParser.cpp`), shared by every parser
instance in every backend slot; they only ever grow. Parser DFA states went 2756 -> 11681
over the 43 files. Parse-only (`--grammar`, no imports, no analysis, tree discarded) still
retains 4 MB -> 71 MB / 821k blocks across those files, which is purely this.
`ParserATNSimulator::clearDFA()` + the lexer's on each reset cut peak 618.7 -> 516.7 MB
(-102 MB) for +10% wall time. `PredictionContextCache` has NO clear API (`put`/`get` only),
so its share is not reclaimable through the runtime.

## What was ruled out (measured, not reasoned)

- **Per-analysis leak.** Same file 60x in one process: flat at 70 MB. `ResetForReanalysis`
  does release per-analysis state.
- **Path-keyed retention.** 43 copies of ONE file under 43 DIFFERENT names: flat at 113.6 MB
  (that file's solo peak). Nothing accumulates per file identity.
- **Unbounded growth.** Same 43 files run TWICE in one process: pass 1 +460 MB, pass 2 only
  +65 MB. Growth saturates once content has been seen.
- **Allocator fragmentation.** Live-block accounting (above) grows with committed, so this is
  live data, not mimalloc holding freed pages.
- **Per-file overhead in general.** 43 tiny distinct files (unique struct + function names,
  no imports): perfectly flat at 51 MB / 673k blocks. Retention scales with analysed volume
  (imports, bound headers), not with file or symbol count.

## Fix direction

Items 1 and 2 LANDED 2026-09-05 (branch `fix/lsp-cache-bounds`). Item 3 is the sole open item.

- **DONE - `cFileSigCache_` row-budget LRU.** Entries carry `lastUse` + `rows`;
  `InsertCFileSigEntry` / `TouchCFileSigEntry` (`LLVMBackend.h`) maintain a running row total
  under `cFileSigCacheMutex_` and evict the least recently used entry while the total exceeds
  `kCFileSigCacheRowBudget` = 120000 rows (about two windows.h bindings). The entry just
  inserted is never the victim, so a single over-budget entry stays. Disk-cache writes still
  happen before the insert, so an evicted `cache`-clause header reloads from disk, not clang.
- **DONE - `parseTreeCache_` ageing.** `CachedParseTree::lastUse` + per-backend
  `parseTreeClock_`; `AgeOutNonCoreParseTrees` runs at the end of `ResetForReanalysis` (after
  `gts.Clear()` drops the ctx pointers) and erases every entry older than
  `kParseTreeMaxAge` = 2 resets whose canonical path is not under `weakly_canonical(runtimeDir/core)`.
  Core trees are kept unconditionally. Measured after the fix: macOS Release bulk sweep
  (`lsp_bulk_test.py`, 181 files, `--lsp-pool-size 4`) peaks at 1.31 GB max RSS.

### Still open

- **Item 3 - ANTLR DFA clearing.** ON HOLD by maintainer ruling 2026-09-05; do not touch the
  parser statics. Clear the parser+lexer DFA on a threshold (state count or files-since-clear),
  not every reset (-102 MB, +10% time if done every time). Capped by the unclearable
  `PredictionContextCache`.
- **Sub-defect: `cFileSigCache_` key dedupe.** Still unfixed and deliberately out of scope of
  the two commits above. The key is `"<canonical .h>|<include dirs>"`, so the same header
  reached under a different include-dir set (or via the `cache` clause) is stored a second
  time - `test_windows_cache.cb` adds a second ~54k-row copy of windows.h. The LRU bounds the
  cost but does not dedupe it.
- **Low: disk-cache write can miss under concurrent eviction.** In
  `LLVMBackend_StateAndImports.cpp` (~2183) the package-header path re-looks up the entry after
  `CompileCHeader` to feed `WriteCHeaderDiskCache`. Another pool thread inserting a large header
  in between can evict it, so the disk write is silently skipped and the NEXT process pays
  clang again. Correctness unaffected. Fix when touched: have `CompileCHeader` hand back the
  entry instead of re-looking it up.
- **macOS measurement 2026-09-05** (181-file sweep, no windows.h bindings, so item 1 never
  fires): peak RSS pool 4 1.29 GB -> 1.25 GB, pool 1 646 MB -> 572 MB. The remaining growth on
  this host is core trees x pool plus the ANTLR statics (item 3).

## Side finding (separate defect, should be its own issue)

`cmake_build.bat release` **exits 0 when the ninja link step fails**. A LNK1169 (duplicate
`operator new`, hit during this investigation) printed `FAILED:` and `ninja: build stopped`,
after which the script still printed `=== Done: x64\release layout ready ===` and left
`%ERRORLEVEL%` at 0, with the previous exe still in place. Any script or agent trusting that
exit code silently tests a stale binary. (A compile-step failure DOES propagate correctly;
only the link/post-link step was observed swallowing it.)

## Instrumentation used (temporary, NOT in the tree)

All added to `LLVMBackend.cpp` and reverted afterwards:
- `CFLAT_HEAP_PROBE=1` - `mi_heap_visit_blocks(mi_heap_main(), true, ...)` summing live block
  bytes/count, called from the top of `ResetForReanalysis` (and from a scope guard in
  `CheckGrammar` for the parse-only measurement). Needs `#include <mimalloc.h>`; mimalloc v3
  has no `mi_heap_get_default`, use `mi_heap_main()`.
- `CFLAT_CACHE_PROBE=1` - sizes of `parseTreeCache_`, `cFileSigCache_` (+ summed row count
  across all its vectors), `cTypedefMap_`, `coreFileNames_`, `paramRetainsMemo_` etc.
- `CFLAT_DFA_PROBE=1|2` - sum `psim->decisionToDFA[i].states.size()` over a throwaway
  `CFlatParser`; `2` also calls `clearDFA()` on the parser and lexer simulators.
- `CFLAT_PURGE_TREES=1` - drop non-core `parseTreeCache_` entries on reset.

Do NOT `#include <windows.h>` in `LLVMBackend.cpp` - its macros collide with the generated
parser's token names.
