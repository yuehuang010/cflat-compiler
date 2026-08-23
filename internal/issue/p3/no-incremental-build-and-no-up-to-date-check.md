# Every build is whole-program: no up-to-date check (case 1) and no per-file reuse (case 2)

Filed 2026-08-21 from an external report (MemPressMonitor Win32 port, v0.11.0 issue 13, last
bullet). Measured on `cd847a3`, Release, on this repo - the two cases below are the actionable
split, because they have DIFFERENT fix costs and case 1 is nearly free.

## Case 1 - NO source file changed: the full build runs again (LANDED 2026-08-22, `8c6ee5e`)

**Done.** `<out>.cflat-dep.json` manifest next to the output; `up to date: <out>` in ~10 ms when
no input/arg/compiler changed; `--force` / `-B` bypasses. What remains in this file is case 2.

There is no up-to-date check at all. Re-invoking with an unchanged input and an existing output
recompiles and relinks from scratch.

Measured (`cflat Test/test_basic.cb -i Test/library -o scratch/triage/inc/basic.exe`, warm cache,
nothing touched between runs):

| Run | Time |
|-----|------|
| build 1 | 2.20 s |
| build 2, no source change | 2.01 s |
| build 3, no source change | 2.04 s |

The reporter measured the same shape at project scale: **no-op rebuild 4.2 s for CFlat against
0.7 s for MSVC** - their widest ratio of any row in the comparison, and the one that is pure waste
rather than a design tradeoff.

This is the cheap half. It needs no incremental compilation and no build-system state: compare the
output's mtime against the mtimes of the input, every transitively imported `.cb`, every bound
header/lib, and the compiler binary itself, plus the flag set (the `--init` cache already does
mtime-keyed invalidation over the core libraries, so the machinery and the precedent both exist).
If everything is older, print "up to date" and exit 0. Gate it behind the absence of a `--force` /
`-B` flag.

## Case 2 - exactly ONE file changed: still the full build

Touching a single imported file costs the same as touching everything:

| Run | Time |
|-----|------|
| build 4, one imported `.cb` touched | 2.00 s |

Identical to the no-change runs, because the work is identical. The reporter's numbers at project
scale: **edit one GUI source, rebuild - 4.2 s CFlat vs 2.1 s MSVC**; **edit one core source - 7.6 s
CFlat (both front-ends rebuild) vs 1.9 s MSVC**. There is no shared-static-library story either, so
a core edit re-costs every front-end that imports it.

This is the expensive half - real incremental compilation (per-module bitcode keyed on content plus
the resolved signatures it depends on, then link) is plan-level work, not an issue-sized fix. File
it, do not start it, and do not let it block case 1.

## Scale, and why this is still p3

At the reporter's ~2.0k lines the absolute numbers are small (~4 s) and they explicitly said it did
not matter in practice; the ratio (~2-6x) is what would matter at 10x the source. Against that,
CFlat needs no configure step and no build-system state at all, which is a genuine and deliberate
simplification. So: case 1 is worth doing on ergonomics grounds soon, case 2 waits for a scale
argument.

## No separate compilation unit either (the other half of case 2)

From the same reporter's README: there is no shared static library, so `memcore` is compiled into
BOTH front-ends and editing one core source rebuilds both exes (7.6 s, against MSVC's 1.9 s to
recompile `grouper.cpp` once and relink). CFlat has no object or library OUTPUT KIND at all -
`doc/CLI.md` offers `-o` (exe), `--out-lli` (`.ll`), `--out-asm` (`.s`), `--bitcode` (`.bc`) and
nothing that another CFlat compile can consume as a prebuilt input.

This is a prerequisite for case 2, not a separate wish: per-file reuse and "build this module once,
link it into N programs" are the same missing mechanism. Whatever design lands must cover both.

## Where the tracking state lives (RULED 2026-08-22)

**Maintainer ruling: incremental tracking information is stored next to the output - derived
from the `-o` path by swapping the extension (e.g. `app.exe` -> `app.cflat-dep` or similar), or
inside the output directory when the output is a directory.** No state under the source tree, no
per-user cache entry, no build-system file the user has to know about. Deleting the output (or its
sidecar) is the whole "clean" story, and two outputs built from the same source never share or
clobber state.

This applies to case 1 immediately: the sidecar records the input, every transitively imported
`.cb`, every bound header/lib, the compiler binary, and the flag set, each with the mtime seen at
the last successful build. A rerun compares the sidecar against the filesystem and exits "up to
date" when nothing is newer. Without a sidecar (first build, or output produced by an older
compiler) the build runs unconditionally and writes one.

Case 2 must use the same location: per-module artifacts (bitcode, resolved signatures) go in the
same sidecar directory keyed off the output path, so the mtime-keyed design above and this placement
rule are the two fixed points any plan has to satisfy.

## Scope and mechanism (RULED 2026-08-22)

**Maintainer ruling: import-level + whole-output incremental is enough. No function-level
hashing.** Two tiers only:

1. **Whole-output up-to-date check** (case 1): sidecar manifest next to `-o`; if nothing is newer,
   exit "up to date".
2. **Per-import reuse** (case 2, module tier): each transitively imported `.cb` gets its own
   `.meta.json` + `.bc` pair in the sidecar, in the SAME format `--init` already writes for
   `core/` (`core_<os>.meta.json` = serialized declarations, `core_<os>.bc` = bitcode). NOTE
   (exploration 2026-08-22, `scratch/inc/import-cache-design.md`): there is NO `linkModules` call
   in the compiler. `LoadCoreBitcodeIfFresh` (LLVMBackend.cpp ~6520) parses the `.bc` into a fresh
   context and ADOPTS it as the module, re-binding tables by name; it works only because exactly
   one cached unit is adopted before any user IR exists. So the first cut of the import tier is
   ONE pair for the root file's whole import closure (reuses adopt-the-module verbatim), and
   true per-file pairs need new `llvm::Linker` plumbing plus named-struct collision handling. An import whose file mtime/hash and transitive-import
   mtimes/hashes match the manifest is loaded from the pair and skips parse, ForwardRefScan and
   codegen entirely; a changed import is rebuilt and its pair rewritten. The manifest is the
   existing `--init` JSON with per-file `{path, mtime, hash}` entries added - no new format.

Why this is the right cut (measured 2026-08-22, `Test/test_basic.cb`, Release, warm cache,
1.17 s total via `-ftime-trace`):

| Phase | ms |
|-------|----|
| Parse (ANTLR) | 238 |
| ForwardRefScan | 200 |
| CodeGeneration (listener -> IR) | 442 |
| OptModule + EmitExecutable (LLVM + link) | 216 |

~75% of the build is the CFlat front end, so the win is in skipping parse/scan/codegen for
untouched imports - which the module tier does. A function-level hash tree (hash each definition's
source span plus the signature hashes of what it references, pull unchanged bodies from cached
bitcode) would only refine the codegen slice inside one touched file and drags in generic
instantiation dedup, `returnBlockTable` / `stringPool` / global-ctor side effects, and a much wider
serializer surface. Not worth it at current scale; revisit only if a single huge file becomes the
bottleneck. LLVM-side IR hashing (ThinLTO-style) is ruled out: it only covers the 216 ms back end.

Prior-art check: Rust's crate = compilation unit with rlib metadata carrying generic definitions
for downstream instantiation, incremental state in `target/` next to the output; Zig = whole-program
with decl-level in-process incremental and a content-hashed cache dir. CFlat has no build manager
(no Cargo), so the compiler is its own fingerprinter and the sidecar is the package; the import
tier is the Rust rlib shape at file granularity, which is what the `--init` core cache already
implements for `core/*.cb`.

Further blockers found by the exploration: field initializers (`Initializer`/`BraceInitializer`
ANTLR pointers) are not serialized (fix: store source text, re-parse lazily, as generic templates
already do); `importCompileDepth_` / `uncertainInterfaceImpls` / `ifConstGuardedImpls_` are
cleared on cache load and never restored; `returnBlockTable` and `stringPool` are not restored;
`FunctionSymbol::Recipe` and `AliasArraySize` / `AliasInnerDims` / `NoaliasScopeId` /
`ParentVariableName` are missing from the serializer. Import graph is a DAG (cycles are an error),
so no SCC unit is needed.

Hazards carried over from `--init` (see `internal/testing-notes.md`): every `TypeAndValue` /
`StructData` / `AnnotationValue` field an analysis reads must round-trip through the serializer,
or a warm sidecar silently drops it for user code too. Generic instantiations emitted into the
importer must be `linkonce_odr` / COMDAT so `linkModules` dedupes them across cached imports.

## Related observation - output is not reproducible (RULED: fine for now)

**Maintainer ruling 2026-08-21: CFlat is not deterministic, and that is accepted for now.** Do not
file it, and do not spend time on it. It is recorded here only because it constrains the DESIGN
space of case 2: a content-hash keyed cache would need determinism, so an mtime-keyed scheme (like
`--init` already uses) is the direction that works with today's behaviour. Case 1 is unaffected.

Measured, for the record:

Two back-to-back builds of the same unchanged source produce exes with different hashes:

```
40A72B91CCA5C57F96AF38A3CF3B57F3F64A8F59DE2164F5FC1111790BB5F2EF
A83C8D628525E0C1027EB7C9D7897A4682985DCB32401DC6CDB1E69CF9F67966
```

Cause not investigated (most likely the PE header timestamp / build id rather than differing
codegen). Prior determinism work is recorded in the completed q16 bucket ("Codegen folding and
determinism").

Adjacent: [[raw-string-literal-prefix]] (p4, formerly papercuts-from-the-mempress-port) (item 4, which this file supersedes).
