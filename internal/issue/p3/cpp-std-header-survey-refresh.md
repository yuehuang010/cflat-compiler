# Refresh the C++ standard header coverage survey

Trailing item from the retired `internal/issue/cppinterop/` folder (deleted 2026-09-30). It held a
2026-09-09 Windows spike (L0 = `import cpp "<header>";` + empty main under `--check`; L1 = a minimal
real use, compiled with `-o` and run) and two stream issues. Every gap it indexed has since landed.

## State 2026-09-30 (macOS arm64 / libc++, master 5a4160ab)
- L0: all 105 headers bound on Windows in the spike.
- Streams: an `std.ofstream` local opens, writes and destructs. `open` binds for `const char*` and
  `std::string`. `Test/test_cpp_interop.cb` leg 3205 asserts this; the leg has no platform guard,
  so Windows `test.bat` runs it as well. That leg is the check for the MSVC-filed
  `basic_ofstream<char>::open` instantiation error.
- `std.cout << ... << std.endl`, `std.pair<int, int>(1, 2)`, `for (int x in v)` over `std.vector`
  and `--cpp-std` all work. Probe: scratch/cppi_probe.cb.

## Remaining
1. Re-run the L0/L1 survey on both hosts and list any new gaps as their own issue files. The old
   L1 list: containers, smart pointers, `function`, `any`, `variant`, `atomic`, `thread`, `future`,
   `condition_variable`, `latch`, `stop_token`, `charconv`, `memory_resource`, streams, `complex`,
   `chrono`, `filesystem`, `tuple`, `optional`, `regex`, `random`, the `c*` headers, `algorithm`,
   `numeric`, `bit`, `limits`, `format`, and the C++23 headers under `--cpp-std c++23`.
2. Headers never spiked, because they have no obvious CFlat surface: `type_traits`, `concepts`,
   `compare`, `ratio`, `iterator`, `ranges`, `coroutine`, `source_location`, `typeindex`,
   `typeinfo`, `initializer_list`, `version`, `execution`, `locale`, `codecvt`,
   `scoped_allocator`, `new`, `exception`, `stdexcept`, `system_error`. They need a maintainer
   ruling on whether they get a CFlat surface at all before a spike means anything.

## Acceptance
A refreshed survey table (host, header, L0, L1) exists in this file, and each failure is filed
separately. Delete this file once the table exists and item 2 has a ruling.
