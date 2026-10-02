# C++ standard library in the test_libs smoke tier

Status: PLAN 2026-10-01, not started.

## Goal (maintainer, 2026-10-01)

Add the C++ standard library to the `test_libs` smoke tier (tier 1), as a new `std` library
directory. Cases are split by C++ standard: C++11, C++14, C++17 and C++20. Each standard covers
the library features it ADDED. C++11 holds the bulk of the library (containers, smart pointers,
algorithms, threads, and so on), because it is the first standard the suite covers.

The suite checks that C++ interop can REACH each feature set correctly (headers ingest, the
CFlat call binds the right overload/specialization, values cross the boundary intact), not that
the STL itself is correct (maintainer, 2026-10-01). One or two calls per feature, each with an
asserted result, is enough; no exhaustive STL semantics.

A leg that does not work today goes into a DISABLED case that names its issue file. That case is
enabled when the fix lands (maintainer, 2026-10-01). Enabled cases stay green.

## Decisions

1. **"C++11" means the feature set; every case compiles at the default `--cpp-std` (c++20)**
   (RULED 2026-10-01). Features removed by a later standard (`auto_ptr`, `bind1st`,
   `random_shuffle`, `std::iterator`, ...) are out of scope.
2. **The cases test the library, not the language.** A language feature (rvalue references,
   variadics, constexpr, concepts, `<=>`) is covered only where a std API exposes it: move-only
   elements, `emplace`, `initializer_list` construction, constrained templates, comparing
   `std::pair` with `<=>`. No helper header with C++ language code.
3. **Each run case has a `.cpp` twin**, as fmt/json do (C++20, `#include <string>` kept per the
   parity-twin ruling). The twin is the reference: it must build with `clang++ -std=c++20` and
   pass before its `.cb` case is written.
4. **Assert only portable values** (the assert proves the value crossed the boundary, not STL
   correctness). Results must agree between libc++ and the MSVC STL. Random:
   check engine output only (`mt19937` 10000th value 4123659995 is standardized), never a
   distribution's output. Unordered containers: check size and lookups, never iteration order.
   `sizeof` of std types: never checked. Floating point: exact only where IEEE guarantees it.
5. **Features unavailable in a host's STL are left out**, not ifdef'd. Example: a libc++ that lacks
   `jthread`, or availability-gated `to_chars(double)` under the macOS deployment target
   (deployment-target ruling: availability errors are real). `if const (__MACOS__)` only when a
   whole leg is platform-bound.

## Runner change (prerequisite, main session, small)

std needs no install root, include dir or lib. Add `root_mac=system` / `root_win=system` to both
`test_libs.sh` and `test_libs.bat`: no root lookup, no probe, no `--c-include`/`--c-lib`, never
skipped. Document it in `test_libs/README.md` (lib.cfg keys + tier table: tier 1 gains `std`).

`test_libs/std/lib.cfg`:

```text
tier=1
root_mac=system
root_win=system
timeout=300
version_mac=clang++ --version
```

## Case layout

`test_libs/std/std_<SS>_<topic>.cb` (+ `.cpp` twin), where `<SS>` is the standard (11, 14, 17, 20);
the README numbering convention is satisfied by the standard number. DISABLED legs live in
`std_<SS>_9N_<topic>.cb`, one per issue, so an enabled case never carries a known failure.

Case rules (README): `GUARDS` + `FROM` markers, assert every printed value, run every leg (no early
return), print the failing leg, return 0 only if all pass. `i64` for 64-bit template arguments.

## Coverage map

### C++11 (bulk)

| Case | Headers / items |
|------|-----------------|
| `std_11_containers` | `array`, `vector` (push/emplace_back, reserve, insert/erase, `initializer_list` ctor), `deque`, `list`, `forward_list`, `map`/`multimap`, `set`/`multiset`, `unordered_map`/`unordered_set` (lookup + size only), `stack`, `queue`, `priority_queue`; iteration with `begin`/`end`; `std::hash` |
| `std_11_strings` | `string` (append, find, substr, compare, `c_str`), `to_string`, `stoi`/`stol`/`stod`, `u16string`/`u32string` sizes, `<cctype>`, `<cstring>`, `<cstdint>` |
| `std_11_memory_utility` | `unique_ptr` (custom deleter, move-only in `vector`), `shared_ptr`/`weak_ptr`/`make_shared`/`use_count`/`lock`, `enable_shared_from_this`; `move`, `swap`, `pair`, `tuple`/`make_tuple`/`get`/`tie`, `forward` via `emplace` |
| `std_11_algorithms_functional` | `<algorithm>`: `sort`, `stable_sort`, `find`/`find_if`, `count_if`, `all_of`/`any_of`/`none_of`, `copy_if`, `transform`, `minmax_element`, `is_sorted`, `lower_bound`, `unique`, `reverse`, `rotate`; `<numeric>`: `accumulate`, `iota`, `inner_product`; `<functional>`: `function` from a CFlat lambda and a free function, `less`/`greater`, `ref` |
| `std_11_numeric_time_random` | `<chrono>` (durations, `duration_cast`, `steady_clock` monotonic), `<ratio>`, `<random>` (`mt19937` / `mt19937_64` raw output only), `<cmath>` (`fma`, `isnan`, `round`, `hypot`), `<complex>`, `<limits>`, `<type_traits>` constants |
| `std_11_concurrency` | `thread` + `join`, `mutex`/`lock_guard`/`unique_lock`, `condition_variable` handshake, `atomic<int>` (`fetch_add` from N threads), `future`/`promise`/`async` (deterministic results only) |
| `std_11_errors_regex` | `system_error`/`error_code`, `exception_ptr` via `current_exception`/`rethrow_exception` if CFlat can call it, `<regex>` match/search/replace on a fixed input. Regex is the heaviest header: move it to its own tier-2 case if it busts the time budget |

### C++14

| Case | Items |
|------|-------|
| `std_14_library` | `make_unique` (incl. array form), `exchange`, `integer_sequence`/`make_index_sequence` size, heterogeneous lookup (`map<string, int, less<>>::find` with `const char*`), `get<T>` on a tuple by type, `cbegin`/`crbegin`/free `rbegin`, four-iterator `equal`/`mismatch`, `shared_timed_mutex`/`shared_lock`, `std::quoted` round trip through `stringstream`, `is_final`/`is_null_pointer` |

### C++17

| Case | Items |
|------|-------|
| `std_17_vocabulary` | `optional` (`has_value`, `value_or`, `reset`, `emplace`), `variant` (`index`, `holds_alternative`, `get_if`, `visit` with a CFlat lambda), `any` (`any_cast`, `has_value`), `string_view` (substr, find, `remove_prefix`, comparison with `string`), `std::byte` |
| `std_17_library` | map/set node API (`try_emplace`, `insert_or_assign`, `extract`, `merge`), `apply`, `invoke`, `not_fn`, `clamp`, `gcd`/`lcm`, `reduce`/`transform_reduce`/`inclusive_scan`/`exclusive_scan` (sequential), `size`/`empty`/`data` free functions, `scoped_lock`, `shared_mutex`, `from_chars`/`to_chars` (integers), `as_const`, `sample` with a seeded engine (size only) |
| `std_17_filesystem` | `path` decomposition (`filename`, `extension`, `parent_path`, `/` join), `exists`/`create_directory`/`file_size`/`remove_all` inside a case-local directory under the case dir, `directory_iterator` count |

### C++20

| Case | Items |
|------|-------|
| `std_20_span_bit_numbers` | `span` (from `vector` and from a CFlat fixed array, `subspan`, `first`/`last`), `<bit>` (`popcount`, `has_single_bit`, `bit_width`, `rotl`, `bit_cast`, `endian::native`), `<numbers>` (`pi`), `midpoint`, `lerp`, `ssize`, `to_array`, `make_shared<T[]>` |
| `std_20_containers_strings` | `contains` on map/set/unordered, `erase`/`erase_if` free functions, `starts_with`/`ends_with` on `string` and `string_view`, `<=>` on `pair`/`string`/`vector` through CFlat comparisons (operator-parity ruling) |
| `std_20_ranges_concepts` | `ranges::sort`/`ranges::find`, `views::iota`/`filter`/`transform`/`take` with CFlat lambdas, pipe composition, a concept-constrained std call accepting a valid type (negative concept checks belong in `Test/errors/`, not here) |
| `std_20_format_chrono` | `std::format` (ints, width, precision, `{:>8}`), `format_to_n`; `chrono` calendar (`year_month_day` arithmetic, `weekday`), `hh_mm_ss` |
| `std_20_concurrency` | `jthread` + `stop_token`, `latch`, `barrier`, `counting_semaphore`, `atomic_ref`, `atomic::wait`/`notify_one` |

## Process

1. Runner change + `lib.cfg` + README (main session).
2. Per standard: write the `.cpp` twin first, build it with `clang++ -std=c++20`, run it. A feature
   the host STL lacks is dropped (decision 5). Then write the `.cb` case leg by leg.
3. A leg failing on a CFlat gap: reduce it, file `internal/issue/p<N>/<name>.md` (check
   `internal/issue/` first, reuse an existing issue), move the leg into
   `std_<SS>_9N_<topic>.cb` with `// DISABLED: internal/issue/...` and its own twin. Never weaken
   the leg to make it pass. When the fix lands, the fixer removes the marker in the same change
   (README rule); the stale-marker check catches a deleted issue with the marker left in.
4. No compiler fixes inside this plan. It only adds cases and files issues.

## Delegation

Main session: step 1, then up to 3 parallel Codex Luna runs (agent concurrency limit), each owning
disjoint files: (a) C++11, (b) C++14 + C++17, (c) C++20. Each brief carries: this plan's
decisions, the case rules, the twin-first order, the DISABLED/issue procedure, the shell-safety
line, the token-efficiency line, "no compiler edits, no edits to Queue.md". Main session owns
Queue.md rows for the filed issues and reviews every new issue file.

## Acceptance

- `./test_libs.sh std` green cold and `--warm` on macOS. `--include-disabled` reports every
  DISABLED case as XFAIL (an XPASS means the marker is stale: enable it).
- Every enabled case's `.cpp` twin builds and passes with `clang++ -std=c++20`.
- Every DISABLED marker names an existing issue file with a repro.
- Time budget: the whole `std` library adds at most ~30 s cold to `./test_libs.sh` at the default
  `-j 4` (warm close to 0). A case over budget moves to tier 2 with a note in this plan. Log the
  measured cold/warm time here.
- `scratch/libs_perf.sh` parity: report the std ratios. They do not gate the landing (std headers
  are a new baseline), but a std case far above the 1.1x target gets a p2 perf issue.
- `./test.sh Release` untouched and green. `test_libs.bat` gets the `system` root; the maintainer
  verifies Windows when the box is back (MSVC STL is the second reference for decision 4).

## Scope

C++11 through C++20 (RULED 2026-10-01). C++23 and later (`expected`, `print`, `flat_map`,
`mdspan`, `ranges::to`) are a possible later extension; they would need a per-case compile-flag
marker (`// CPPSTD: c++23`) in the runner.
