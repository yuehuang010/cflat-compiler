# C++ standard library header coverage: spike survey and gap index

Spiked 2026-09-09 on Windows (`x64/Release/cflat.exe`, MSVC STL via `import cpp`, clang pinned to
`-std=c++20`). Two levels: L0 = `import cpp "<header>";` + empty `main` under `--check`; L1 = a
minimal real use of the header's primary facility, compiled with `-o` and run.

This file is the survey and the index. Each gap has its own issue file with repro, root-cause
notes and acceptance.

## L0: all 105 headers bind. Zero failures.

Header discovery, the MSVC toolset include scan and the parse path are not the bottleneck - what
fails is reaching the declarations inside.

## L1: works today (built and ran, exit 0)

`vector` `array` `deque` `list` `forward_list` `map` `set` `unordered_map` `unordered_set`
`queue` `stack` `span` `bitset` `valarray` `string` `string_view` `memory` (`unique_ptr`,
`shared_ptr`) `functional` (`std.function` over a CFlat function) `any` `variant` `atomic`
`thread` `future` (`promise`) `condition_variable` `latch` `stop_token` `charconv`
`memory_resource`.

Container construction (`= default`), `push_back` / `push` / `operator[]` / `size` / `find`,
member calls on a `T&` element, and `auto it = m.find(1)` all work. `optional` and `pair` work
when declared `= default`; their constructor-call spelling does not (gap 4).

## L1: gaps

| # | Issue | Blocks |
|---|-------|--------|
| 1 | [`std-free-functions-and-globals-unreachable.md`](std-free-functions-and-globals-unreachable.md) | all 21 `c*` headers, `algorithm`, `numeric`, `bit`, `limits`, `format`, `iostream` globals |
| 2 | [`cpp-alias-template-types-unresolvable.md`](cpp-alias-template-types-unresolvable.md) | `std.ofstream`, `std.ostringstream` |
| 3 | [`stream-classes-no-callable-destructor.md`](stream-classes-no-callable-destructor.md) (destructor half FIXED 2026-09-10; blocked now by [`cpp-virtual-base-constructor-unreachable.md`](cpp-virtual-base-constructor-unreachable.md) and [`stream-open-instantiation-error.md`](stream-open-instantiation-error.md)) | streams as locals; with 1+2, the whole iostream family |
| 4 | [`constrained-template-constructor-overload-resolution.md`](constrained-template-constructor-overload-resolution.md) | `complex`, `chrono`, `filesystem`, `tuple`, `optional`, `pair`, `regex`, `random` construction |
| 5 | `lock` as a member name - LANDED `6f1981c` | was: `std.mutex.lock`, `std.shared_mutex.lock` |
| 6 | [`range-for-over-cpp-container.md`](range-for-over-cpp-container.md) | `for (T x in c)` over every C++ container - PARKED 2026-09-09 by maintainer ruling, awaiting a protocol decision (size/`[]` vs begin/end vs both) |
| 7 | [`no-cpp-standard-selection-flag.md`](no-cpp-standard-selection-flag.md) | `expected`, `flat_map`, `flat_set`, `generator`, `mdspan`, `print`, `stacktrace`, `stdfloat` |
| 8 | multi-word template arguments - LANDED `6f1981c` in template-argument position only; gap 10 extends the spellings to every type position | was: any specialization over `long long`, `unsigned int`, ... |
| 9 | [`std-globals-unreachable.md`](std-globals-unreachable.md) | `std` GLOBALS (`std.cout`); the narrowed remainder of gap 1 |
| 10 | [`cxx-primitive-typing.md`](cxx-primitive-typing.md) | C multi-word spellings as aliases in every type position, plus `c8`/`c16`/`c32`/`wchar` (`std::u16string`, `std::wstring`, `char8_t` specializations) - maintainer ruling 2026-09-10 |
| 11 | [`alias-of-class-template-specialization-loses-fields.md`](alias-of-class-template-specialization-loses-fields.md) | an alias of a specialization declares a local but loses its fields |
| 12 | [`c-long-has-no-cflat-cxx-spelling.md`](c-long-has-no-cflat-cxx-spelling.md) | C++ `long`/`unsigned long` unreachable (`std::vector<DWORD>`, `std::vector<size_t>`); first slice of gap 10 |

## Not spiked

`type_traits` `concepts` `compare` `ratio` `iterator` `ranges` `coroutine` `source_location`
`typeindex` `typeinfo` `initializer_list` `version` `execution` `locale` `codecvt`
`scoped_allocator` `new` `exception` `stdexcept` `system_error` `cstddef` `cstdint` `climits`
`cerrno` and friends have no CFlat-reachable runtime surface beyond gap 1, or exist only to be
consumed by C++ code. They bind at L0; they need a ruling on whether they should have a CFlat
surface at all before a spike means anything.

## Sequencing

Gaps 1, 5, 6, 8 are independent and small. Gap 1 unlocks the most headers per unit of work and
should go first. Gaps 2 and 3 are one story - neither alone makes a stream usable. Gap 4 is the
deepest: constructor overload ranking over constrained member templates, the same machinery the
working `const char*` constructor of `std.string` already exercises. Gap 7 needs a maintainer
ruling on the target standard before it is a flag.

Gap 3 split on 2026-09-10: the virtual destructor is fixed (a clang-emitted thunk carries the
dispatch for a vfptr inside a virtual base); the constructor and `open` rungs behind it are filed
separately and must be closed before a stream can be a local.

Delete this file when all eight are closed.
