# C++ standard library, comprehensive tier-2 suite (`std_full`)

Status: BUILT 2026-10-02 (macOS only). 44 enabled + 53 DISABLED cases in test_libs/std_full/, legs
356 enabled / 89 DISABLED (80% pass). `./test_libs.sh -t 2 --include-disabled --warm std std_full`: PASS=82
FAIL=0 XPASS=0 XFAIL=53, 53 s cold+warm. 46 issues filed (36 p2, 10 p3), Queue.md "std_full tier-2 gaps".
API-family reach (266 rows, 236 testable): tier 1 alone 45% yes-or-partial, tier 1+2 96% (73% full);
headers 45/85 -> 71/85. Table: scratch/std_cov/coverage_table.md. MSVC STL not yet verified.

Tier 1 (`test_libs/std/`, plan std-library-smoke-suite.md) reaches each C++11-C++20 feature set with 1-2 calls. This tier covers every non-deprecated C++11-C++20 header and
every API family within each header that CFlat can reach.

## Goal (maintainer, 2026-10-02)

- New library `test_libs/std_full/`, `tier=2`: runs on request (`./test_libs.sh -t 2 std_full`) and
  nightly (`buildci.sh --nightly` runs tiers 1-3). No tier 3 for std: tier 3 is for minutes-per-case
  libraries (torch).
- C++11-C++20 only (RULED 2026-10-02). C++23 is a later follow-up set (needs a `// CPPSTD: c++23`
  runner marker); libc++ availability is recorded in scratch/std_tier2_coverage_map.md.
- TEST CHANGES AND ISSUE FILES ONLY (maintainer, 2026-10-02). No compiler, core, runner or locale
  edits. A gap is filed and its leg moved into a DISABLED case (tier-1 process, RULED 2026-10-02).

## Decisions

Tier-1 decisions 1-5 apply unchanged (feature set at default `--cpp-std` c++20, library not
language, `.cpp` twin first, portable asserts only, features missing from a host STL left out).
Depth is still "interop reaches it and values cross the boundary", not STL correctness: 2-4 calls
per API family, each asserted.

Left out on macOS (decision 5): execution policies and `osyncstream` (libc++ needs
`-fexperimental-library`), `<cuchar>` conversions, `mbrtoc8`, `atomic<shared_ptr>`, chrono time
zones / `utc_clock` / `parse`, `is_layout_compatible`, `<cmath>` special functions.

Left out as MSVC-nonportable (decision 4): the `std::barrier` completion function in
std_full_20_sync_primitives. A `std::function<void()>` completion is not nothrow-invocable, which
MSVC STL static_asserts (libc++ accepts it); the case uses `std::barrier<>` with no completion.

Not testable from CFlat (no case, no issue): `<cstdarg>`, `<csetjmp>`, throw/catch,
`nested_exception`, `rethrow_exception`, custom allocator / streambuf / facet / formatter classes,
a coroutine handle to a real coroutine, UDL literals. A leg that needs a C macro the bridge does not
import (`INT_MAX`, `errno`, `FE_*`, `PRId64`, `__cpp_lib_*`) is a GAP: file it, do not hard-code
the value.

## Case layout

`test_libs/std_full/std_full_<SS>_<topic>.cb` + `.cpp` twin; `<SS>` = standard of the header or the
cluster's newest member. DISABLED legs: `std_full_<SS>_9N_<topic>.cb` (+ twin), one per issue.
Case rules from test_libs/README.md (GUARDS + FROM, assert every printed value, run every leg, print
the failing leg, return 0 only if all pass, `i64` for 64-bit template args).

## Coverage map

Full per-header family table: scratch/std_tier2_coverage_map.md (2026-10-02). Case list:

| Group | Cases |
|---|---|
| A language / utility / memory | 01 language_support, 02 cstdlib, 03 new_initializer_list, 04 exception_stdexcept, 05 typeinfo_typeindex, 06 diagnostics_c, 07 system_error, 08 type_traits_ratio, 09 utility_tuple, 10 functional, 11 memory_smart_ptrs, 12 memory_raw, 13 scoped_allocator, 14 bitset |
| B containers / algorithms | 15 sequence_containers, 16 vector_bool, 17 associative_containers, 18 unordered_containers, 19 container_adaptors, 20 iterator, 21 algorithm_nonmodifying, 22 algorithm_modifying, 23 algorithm_order, 24 numeric, 37 vocabulary_charconv (17), 38 memory_resource (17), 42 bit_numbers_compare_span (20) |
| C1 strings / numerics / time | 25 strings, 26 c_strings, 27 cmath_cfenv, 28 complex_valarray, 29 random, 30 chrono_ctime, 33 cstdio, 34 locale |
| C2 I/O / concurrency / C++17-20 | 31 iostreams, 32 file_and_string_streams, 35 regex, 36 concurrency_full, 39 filesystem (17), 40 format (20), 41 ranges (20), 43 coroutine_source_location_version (20), 44 sync_primitives (20) |

Item lists per case: coverage map, "Proposed tier-2 case list".

## Process

1. Main session: `test_libs/std_full/lib.cfg` (tier 2, system root) + README tier table.
2. Per case: `.cpp` twin first, `clang++ -std=c++20`, run it; then the `.cb` leg by leg.
3. A leg failing on a CFlat gap: reduce to a standalone repro, check `internal/issue/` for an
   existing issue (reuse it), else file `internal/issue/p<N>/<name>.md` (summary, repro, root-cause
   guess marked as a guess, fix direction). Move the leg into a `9N` DISABLED case. Never weaken a
   leg to pass.
4. Main session owns Queue.md rows, deduplicates issues filed by parallel groups, reviews every issue.

## Delegation (2026-10-02)

Codex Luna capped until 2026-10-06: Sonnet implements (Agent tool), fresh Opus reviews. Max 3
implementers on the Mac. Wave 1: A, B, C1 in parallel (disjoint files). Wave 2: C2. No Windows box
this session: Mac only; MSVC STL verification when the box is back.

## Acceptance

- `./test_libs.sh -t 2 std_full` green cold and `--warm` on macOS; `--include-disabled` shows every
  DISABLED case as XFAIL.
- Every enabled twin builds and passes with `clang++ -std=c++20`.
- Every DISABLED marker names an existing issue file with a repro.
- Budget: std_full cold at default `-j 4` logged here; target <= ~5 min cold (nightly).
- `./test.sh Release` untouched (no compiler change).
