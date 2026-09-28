# Real-library C/C++ interop suite (`test_libs.sh`)

Status: BUILT 2026-09-26 (uncommitted): `test_libs.sh`, `test_libs.bat` (unrun - no Windows host),
`test_libs/` json / fmt / simdjson / eigen / torch, tier-1 LIBS stage in `buildci.sh` / `buildci.bat`.
Deviations from the map below: Eigen became 8 cases (eigen_02 enabled with explicit spellings;
05 implicit expression init, 06 `block` int-literal ambiguity, 07 QR solve split out DISABLED, two
new p3 issues); torch_01 spells t27's index with an explicit TensorIndex vector and the braced form
moved into DISABLED torch_90 (init-list issue manifests there). Phases 1 (C libs), 3 (tier 4
probes), 5 (Windows provisioning beyond deps/env roots) remain. Migrates the header-ingestion spikes (libtorch
ladder, simdjson, Eigen/fmt/json probes) into a tracked, tiered regression category.

Scope ruling (maintainer, 2026-09-26): the suite is about INGESTING HEADERS of prebuilt
libraries, giants included (revised same day: libtorch is in, as tier 3 - it is nowhere near
LLVM / Chromium scale, which is the size ceiling that stays out). Out of scope: anything
built from source by the compiler (Dear ImGui via `import cpp "*.cpp"`) - that spike stays in
`scratch/` as manual validation.

## Goal (maintainer, 2026-09-26)

A separate test category that compiles and runs CFlat programs against REAL C and C++ libraries,
so a compiler change that breaks real-world interop is caught by a script, not rediscovered weeks
later by re-running a spike (the 2026-09-26 re-validation found 4 libtorch regressions/latent bugs
and ~20 simdjson/Eigen/fmt/imgui gaps nobody had run for).

## Relation to the 2026-09-15 "no third-party libs in tests" ruling

That ruling stands for the main suite and is NOT relaxed:

- `test.sh` / `test.bat` / `Test/` stay third-party-free. Every bug a library case finds is still
  reduced to a standalone repro in `Test/library/cpp_interop_*.h` (or a C header) when fixed.
- The library suite is a SEPARATE script, like `test_example.sh`. It detects the regression; the
  in-repo fixture pins the fix. A library case never substitutes for the fixture leg.
- A missing library is a SKIP, never a FAIL (fresh clone and other hosts stay green).

## Decisions (maintainer, 2026-09-26)

1. **Tier 1 is required to LAND** (part of the pre-ff-merge bar and of CI), but NOT part of the
   dev loop - do not run it on every edit, and never fold it into `test.sh`.
2. **Location: top-level `test_libs/`**, not under `Test/`.
3. **CI:** tier 1 runs as its own `buildci.sh` stage (after TESTS, before EXAMPLES), not inside
   `test.sh`. Tiers 2/3 cadence is decided after the duration measurements below.

## Layout

```
test_libs/                          # top level; outside Test/, so test.sh never sees it
  README.md                         # how to provision, run, add a case, XFAIL rules
  <lib>/lib.conf                    # sourceable manifest, see below
  <lib>/<lib>_<nn>_<topic>.cb       # one self-checking program per feature cluster
  <lib>/support/                    # optional: small .c/.cpp helpers, data files
test_libs.sh                        # runner (macOS/Linux); test_libs.bat later (phase 5)
```

`lib.conf` (shell-sourceable, one per library):

```bash
LIB_TIER=1
LIB_KIND=cpp                                # c | cpp
LIB_PROBE="/opt/homebrew/opt/fmt/include/fmt/format.h"    # exists -> library present
LIB_VERSION_CMD="brew list --versions fmt"  # recorded in the summary, never compared
LIB_FLAGS="--c-include /opt/homebrew/opt/fmt/include --c-lib /opt/homebrew/opt/fmt/lib/libfmt.dylib"
LIB_RUNENV=""                               # e.g. DYLD_LIBRARY_PATH=/opt/homebrew/opt/simdjson/lib
LIB_INSTALL_HINT="brew install fmt"
LIB_CASE_TIMEOUT=120
```

Paths come from `brew --prefix <formula>` at runner start, not hard-coded, when the formula is
brew-provided; nlohmann-json comes from the compiler's own vcpkg tree
(`~/.cflat-compiler-deps/vcpkg_installed/<triplet>/include`), so it is present on every build host.

## Case rules

0. **Header ingestion is the signal.** Each library gets one `<lib>_00_ingest.cb` that imports the
   umbrella / public headers and touches nothing, compiled with `--check`: it fails when a header
   that used to bind stops binding. Feature cases then use the bound types and functions and link
   + run only as far as needed to prove the ABI (layout, calling convention, exceptions) - keep
   runtime logic minimal.

1. **Self-checking.** `extern int main()` returns 0 on success, nonzero with a printed reason on
   mismatch. Expected values are computed by the program or hard-coded from a clang++ reference
   run (record the reference command in a header comment). No golden-output files.
2. **One feature cluster per case**, compile time budget per tier (below). Merge ladder rungs that
   exercise the same thing; a case that fails must point at one area.
3. **XFAIL is tied to an issue file.** First line `// XFAIL: internal/issue/<bucket>/<name>.md`.
   Runner: case fails -> XFAIL (not counted as failure); case passes -> XPASS = FAILURE ("remove
   the marker, the fix landed"); marker names a missing file -> FAILURE ("issue deleted, marker
   stale"). This keeps the suite green by default and forces bookkeeping on every fix.
4. **Header comment** names what the case guards (feature, and the commit or issue that motivated
   it), so a failure is triageable without history digging.
5. **ASCII, `.cb` only, current spellings** (e.g. `<i64>` not `<long>` - declared-identity
   spelling 6953f4d4 made the old spikes stale). No `scratch/` paths, no absolute paths in sources.

## Runner (`test_libs.sh`)

```bash
./test_libs.sh                   # tier 1, Release
./test_libs.sh -t 2              # tiers 1..2
./test_libs.sh -t 3 torch        # tiers 1..3, only the torch directory
./test_libs.sh --warm            # second pass on the warm cache, checks budget (below)
./test_libs.sh --strict          # missing library = FAIL (for a provisioned CI host)
```

- Reuse `test.sh` machinery: config arg / `CFLAT_CONFIG`, `--init-local`, parallel pool, per-case
  timeout, PASS/FAIL/SKIP/XFAIL/XPASS summary, Elapsed + load line.
- Own `CFLAT_CACHE_DIR` under `out/libs-cache/` (header + request cache), so runs never
  touch the user cache and the warm pass is meaningful.
- Summary prints each library's version (`LIB_VERSION_CMD`) so "compiler regressed" vs "brew
  upgraded the library" is visible at a glance.
- `--warm`: rerun every PASS case against the populated cache; enforce the header-parse budget
  (2026-09-22 ruling: cold = 1 TU, warm = 0) and flag a warm case slower than its cold run.
- Exit 1 on any FAIL / XPASS / stale marker.

## Tiers

Cost numbers are Release, macOS arm64, measured 2026-09-26 (cold per case).

### Tier 1 - smoke (every C/C++ interop change; target < 90 s total)

| Library | Kind | Source on host | Status 2026-09-26 | Guards |
|---|---|---|---|---|
| nlohmann-json | C++ header-only | vcpkg tree (always present) | 10 probes: 9 pass, j03 XFAIL (converting ctors) | templates, operator[], iterators, exceptions, ADL |
| fmt 12 | C++ prebuilt lib | brew `fmt` | 6 probes: 3 pass; f02/f04/f05 XFAIL | variadic templates, consteval, [[no_unique_address]], std::string |
| simdjson 4 | C++ prebuilt lib | brew `simdjson` | s0/s1 pass; o1-o8 fail (issues filed) | simdjson_result<T>, namespace aliases, arch-specific namespaces |
| sqlite3 | C | brew `sqlite` | not probed on macOS (Windows example/vcpkg/sqlite_demo.cb exists) | callbacks with void*, char**, opaque handles |
| zstd / lz4 | C | brew | not probed | size_t APIs, macro constants, buffers |

### Tier 2 - template and API-surface stress (per review batch / before ff-merge of interop work; target < 10 min)

| Library | Kind | Source | Status | Guards |
|---|---|---|---|---|
| Eigen 5 | C++ header-only, heavy templates | brew `eigen` | 10 probes: 3 pass; 7 XFAIL (inherited statics/operators, expression-template operators) | CRTP, expression templates, fixed/dynamic sizes; 15-60 s per case |
| libuv | C | brew | not probed | callbacks, unions in structs, platform typedefs |
| pcre2 | C | brew | not probed | macro-generated names (`PCRE2_SUFFIX`), code-unit width variants |
| Lua 5.4 | C | brew `lua@5.4` | not probed | embedding API, C function pointers called back from C, varargs |
| simdutf / ada-url | C++ prebuilt | brew | not probed | modern C++20 APIs, std::string_view, result types |
| abseil | C++ prebuilt, large headers | brew | not probed | inline namespaces (`absl::lts_*`), heavy SFINAE, flat_hash_map |

### Tier 3 - giant (nightly / timebox close; tens of minutes)

| Library | Kind | Source | Status | Guards |
|---|---|---|---|---|
| libtorch 2.14 | C++ prebuilt, huge headers | brew `pytorch` | 31 rungs: 28 pass; t4/t13/t17 fail + t23 wrong value (issues filed) | the whole bridge: unique/shared, overload ranking, exceptions, tensor ops; 35-175 s per rung cold |

### Tier 4 - candidates, not yet probed (probe before promoting)

OpenSSL 3 (macro-defined functions, opaque structs), SDL3 (big C header; init/version only,
headless), OpenBLAS / CBLAS (Windows example/vcpkg/blas_gemm.cb exists), harfbuzz / freetype
(callback-heavy C), mimalloc; protobuf (needs protoc-generated code checked into the case's
`support/`). Measured 2026-09-26:
- Halide 21 (brew): headers are ONE amalgamated `Halide.h` (35.6k lines, std headers only, no
  LLVM headers). `libHalide.dylib` links brew `llvm@21` (a second LLVM, not cflat's pinned one) -
  link/run cost only. Cases must use `-o`, never `--run` (two LLVMs in one process).
- ONNX (brew `onnx` 1.22): the model-format library, not the inference engine - 1.6 MB / 105
  header files, 4.9 MB lib, headers are protobuf-generated `.pb.h` (so this doubles as the
  protobuf case). ONNX Runtime (brew `onnxruntime`, not installed) is the engine: C API through
  a function-pointer table (`OrtApi`) plus a header-only C++ wrapper - a better ingest target;
  probe before choosing.
- For scale: libtorch headers are 56 MB / 9,776 files. Promote after a probe shows the
ingest cost.

Excluded by the scope ruling: Dear ImGui (built from source).

## Migration map (spike -> case)

| Spike | Target | Notes |
|---|---|---|
| `scratch/probe3/j01-j10` | `test_libs/json/json_01..` | merge j05+j06, j07+j09; j03 XFAIL -> plan converting-constructors |
| `scratch/ladder/json/j1,j2` | fold into json cases | duplicates of probe3 coverage |
| `scratch/probe3/f01-f06`, `ladder/fmt/f1,f2` | `test_libs/fmt/` | f02 XFAIL consteval issue; f04 XFAIL no_unique_address; f05 XFAIL converting-constructors |
| `scratch/simdjson_spike/s0,s1,s1_i64,o1-o8` | `test_libs/simdjson/` | drop s1_cache (removed `cache` clause); s2/s3 rewritten with `<i64>`; o-series XFAIL per filed issues |
| `scratch/ladder/torch/t1-t31` | `test_libs/torch/` ~8-10 cases | consolidate by cluster (construction, arithmetic, autograd, nn modules, optim, save/load, exceptions, shared/unique); list the rungs each case replaces in its header; t4/t13/t17/t23 XFAIL per filed issues; t17 save path in a temp dir |
| `scratch/probe3/e01-e10`, `ladder/eigen/e1` | `test_libs/eigen/` | XFAIL: inherited-statics, operator-returning-specialization |

Every XFAIL above must name the exact issue file; the migrating agent reads `internal/issue/` for
the current names, never this table (names drift).

## Consolidation (2026-09-26 proposal: 67 spike programs -> 24 cases + 4 new ingest cases)

### Rules that keep coverage while merging

1. **A compile error kills every leg in the file.** So a case merges only legs that compile today.
   Each XFAIL stays a small separate case until its fix lands; the fixing change folds it into its
   group (and deletes the XFAIL file) in the same commit.
2. **No early return.** Every leg records into a failure bitmask and prints its own
   `FAIL <leg>: got X want Y`; `main` returns the mask. One wrong value never hides the others.
3. **Assert everything printed.** j09 printed `str=0` and still exited 0 - that is the
   intrinsic-name hijack bug (p2 intrinsic-names-hijack-member-method-calls), silently passing.
4. **Spelling variants are legs, not duplicates.** simdjson o1-o8 and torch t22/t25 differ only in
   spelling, but each spelling is a different binding path (namespace alias vs arch namespace,
   explicit `padded_string_view` cast, pointer+length overload; `OrderedDict::front()` vs
   `operator[]`). Keep every spelling as a leg.
5. **An XFAIL may name several issues**; it XPASSes only when all are fixed.

### Map

| Case | From | State |
|---|---|---|
| json_00_ingest | new: `#include <nlohmann/json.hpp>` only, `--check` | pass |
| json_01_read | j01 key, j02 at/contains, j04 array/push_back, j05 int index, j06 get<string>, j07 dump(2), j08 iterator loop, j10 accept | pass |
| json_02_type_checks | j09 is_number/is_string/is_null/is_object, asserted | XFAIL intrinsic-names-hijack (runtime) |
| json_03_build | j03 `j["x"] = 3`, `j["name"] = "bob"` | XFAIL plan converting-constructors |
| fmt_00_ingest | new | pass |
| fmt_01_runtime | f01 format(runtime), f03 to_string, f06 print | pass |
| fmt_02_consteval | f02 `fmt.format("{{}}", ...)` | XFAIL consteval-format-string |
| fmt_03_memory_buffer | f04 | XFAIL no-unique-address |
| fmt_04_string_arg | f05 `std.string name = "world"` | XFAIL converting-constructors |
| simdjson_00_ingest | s0 (already import-only) | pass |
| simdjson_01_dom | s1_i64 `dom.parser`, `at(2).get_int64()` | pass |
| simdjson_02_ondemand | o1-o8 as legs, `<i64>` spelling | XFAIL namespace-alias + result-member-at; o1/o2/o5 unattributed - re-probe with `<i64>`, file an issue if still failing |
| eigen_00_ingest | new: `<Eigen/Dense>` | pass |
| eigen_01_basics | e01 fixed ctor, e03 dynamic + setOnes, e06 dot/norm | pass |
| eigen_02_decomp | e08 transpose/inverse, e09 QR solve, e10 block/col - REWRITTEN to build via `setZero()` / `setIdentity()` so they stop depending on the statics bug | probe first; may pass today (coverage gained) |
| eigen_03_statics | e02 `Identity()`, `Zero()` | XFAIL inherited-static-members |
| eigen_04_operators | e04 `a + b`, e05 `m * v`, e07 `a * 2.0` | XFAIL inherited-member-operators + operator-returning-specialization |
| torch_01_tensor | t1 ones, t2 add+alpha, t3 `t + t`, t10 op breadth, t20 cat/stack/zeros_like, t27 index/Slice | pass |
| torch_02_autograd | t5 SGD by hand, t26 xavier init + clip_grad_norm_ | pass |
| torch_03_modules | t6 Linear + from_blob, t11 Conv2d/max_pool, t19 BatchNorm/Dropout/eval, t24 LSTM + std::tuple | pass |
| torch_04_training | t7 SGD loop, t9 MLP + Adam, t12 Sequential variadic, t22 + t25 named_parameters (front / []), t28 register_module | pass |
| torch_05_data | t14 loader batches, t15 map(Stack) pipeline, t16 end-to-end | pass |
| torch_06_serialize | t8 tensor save/load, t18 Embedding + module save/load, t21 cross-entropy with Long targets | pass |
| torch_07_cpp_struct | t29 [cpp] Module, t30 make_shared, t31 generic [cpp] struct | pass |
| torch XFAIL singles | t4 (initializer-list begin/end, incremental), t13 (data Example specialization), t17 (T&& scalar return), t23 (initializer evaluated twice, wrong value) | XFAIL each, fold into 02 / 05 / 06 / 06 on fix |

Counts: 67 programs (json 10, fmt 6, simdjson 10, Eigen 10, torch 31) -> 24 cases, plus 4 new
ingest cases = 28: 16 passing (eigen_02 pending its probe), 12 XFAIL. When every XFAIL is fixed
and folded into its group: 16 cases. Nothing is dropped - every program above maps to a leg.

Expected saving is largest in tier 3: libtorch costs 19-175 s PER COMPILE, warm or cold, so 31 -> 7
compiles (+4 XFAIL singles) should cut the ~31 min serial compile roughly in half or better. Measure
after merging; a merged case > 5 min cold gets split along its legs.

## Phases

0. **Runner + tier 1 json.** `test_libs.sh`, `test_libs/README.md`, `json/` cases. Acceptance:
   green on this host, SKIP path proven by pointing one `LIB_PROBE` at a missing file, XFAIL /
   XPASS / stale-marker paths each proven once (then reverted).
1. **Rest of tier 1**: fmt, simdjson, sqlite3, zstd/lz4 (probe the C libs first, write cases for
   what works, file issues for what does not - same file-issue discipline as 2026-09-26).
2. **Tier 2**: Eigen, then the unprobed libs one at a time (probe, case what works, file issues).
3. **Tier 3**: torch consolidation (verify each case covers the rungs it replaces), then tier 4
   probes, promoting candidates that ingest at acceptable cost.
4. **Cadence wiring**: `buildci.sh` LIBS stage (tier 1, `--strict` off so a host without a
   library SKIPs); one AGENTS.md line: tier 1 before any ff-merge, never in the dev loop;
   `--warm` in tier 1.
5. **Windows parity** (later, maintainer-gated): `test_libs.bat`, provisioning through
   `import package-vcpkg` / a separate manifest. Root `vcpkg.json` is NOT touched without explicit
   permission - a library-suite manifest must live elsewhere.

## Durations (measured 2026-09-26, Release, M-series 18 cores, load ~3)

Fresh `CFLAT_CACHE_DIR` (cold, after `--init`), then the same cases again (warm). Script:
`scratch/libtime/run.sh`. Failing cases fail fast (0.2-30 s), so numbers GROW as XFAILs get fixed.

| Tier | Cases today | Cold wall (-j4) | Cold serial sum | Warm wall | Projected cold when all pass |
|---|---|---|---|---|---|
| 1: json + fmt + simdjson | 27 (15 pass) | 12 s | 45 s | 1 s | ~20-30 s |
| 2: Eigen | 10 (3 pass) | 67 s | - | 2 s | ~2-3 min |
| 3: libtorch | 31 rungs (28 pass) | ~11 min (-j3, 2026-09-26 run) | 1869 s | 19-96 s PER RUNG (spot check t1/t10/t5) | ~5-10 min after consolidation to 8-10 cases |

Notes:
- For comparison `test.sh Release` is ~125 s warm; tier 1 adds < 25% even cold.
- Tier 1 and 2 collapse to seconds on a warm cache; CI keeps `out/libs-cache/` between runs, so
  the steady-state CI cost is the warm column.
- libtorch does NOT collapse when warm (19-96 s per rung): the cost is codegen / link against the
  large libraries, not header parsing. Not investigated - check it before tier 3 enters any
  regular cadence (possible issue for the header cost model).

## Open questions for the maintainer

1. Tier 2 / tier 3 cadence, once durations are known (see Durations).
