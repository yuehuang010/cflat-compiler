# --run C++ runtime gaps left after WINRUN (Windows link failures, macOS exit() teardown)

WINRUN (box 7, 2026-10-31) fixed Windows --run of C++ code with exit-time destructors: on master it failed to
LINK (`.xdata` __CxxFrameHandler3/4 / __GSHandlerCheck relocations out of range, missing MSVC thread-safe-statics
symbols `_Init_thread_header` family). The SEH plugin now routes every `.xdata` handler through an in-image jump,
host shims provide the thread-safe statics, and a per-session atexit/_onexit registry runs on main return and on
exit(). Probes match the -o exe (exit code, destructor output, order). Remaining:

- Fixed in WINRUN2: Windows --run of C++ `throw` / try-catch (local, rethrow, catch(...), a throw crossing a
  CFlat frame caught in C++, std::out_of_range from vector::at), std::vector / std::string, RTTI (dynamic_cast,
  typeid().name()) and over-aligned new now link and match the -o exe. The type_info vftable, the operator
  new/delete family (sized, aligned, nothrow, aligned-nothrow), `std::nothrow` and `__type_info_root_node` bind to the host static C++ runtime;
  `_CxxThrowException` is a host shim that raises the record with the JIT graph's image base (RtlPcToFileHeader
  cannot see JIT memory); a C++ exception escaping main terminates with 0xC0000409 like the exe.
- Windows has no C++ --run test host: test.bat's only --run is cli_defines_fixture (pure CFlat). A leg for this
  area needs a new test.bat block (main-session decision).
- macOS --run: exit() called from JIT code may skip the ORC `__cxa_atexit` destructors (main return runs them).
  Not measured. Probe: header `inline` object with a printing dtor + CFlat `exit(4)` under --run vs -o.
- PRE-EXISTING (before | fix | exe): Windows --run of std::stable_sort (repro: `std::vector<int>` + `std::stable_sort`)
  still fails to link on `__std_rotate` (msvcp vectorized-algorithm export, no DLL in the /MT host has it; other
  `__std_*` algorithms likely the same) | before: link fail (+ type_info/sized-delete/nothrow) | fix: link fail
  `__std_rotate` only | exe: prints the sorted median, rc 0.
- PRE-EXISTING (before | fix | exe): C++ `new int` released by CFlat `delete` - before --run 0xC0000374 heap
  corruption | fix --run 0xC0000374 | exe rc 0. The two `new`/`delete` families use different allocators.
- PRE-EXISTING (before | fix | exe): CFlat `new int` released by C++ `delete` - before --run link fail (sized
  delete) | fix --run nondeterministic (prints ok, 1 of 3 runs 0xC0000005) | exe 0xC0000374 heap corruption.
  Cross-language new/delete mixing is undefined in both modes; needs a ruling (refuse, or route both to one heap).
  RULED 2026-11-04 (maintainer): for now TWO families - CFlat `new`/`delete` (active-allocator slot) and C++ `new`/`delete`
  (global operator new) must not be mixed for scalar / CFlat types, a documented error with no diagnostic (like malloc vs new
  in C++); C++ class types already pair. Target is ONE family, so C++ code allocates from a `program` thread's allocator:
  internal/plan/cpp-one-allocator-family.md. Test consequence: none now; one LANGUAGE.md C++-interop line owed.
- PRE-EXISTING: cruntime.cb declares `extern int atexit(void*)`, so `atexit(fa)` with a CFlat function needs `(void*)fa`.
