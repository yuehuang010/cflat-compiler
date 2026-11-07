# One allocation family for CFlat and C++ new/delete

Status: PLANNED (ruled 2026-11-04, maintainer). Until it lands, the two families stay separate and mixing them for
scalar / CFlat types is a documented error (p3/run-cxx-runtime-link-and-exit-gaps).

## Ruling

The maintainer wants ONE `new`/`delete` family, so C++ code works inside a `program` construct: a C++ `new` executed on
a program thread allocates from that program's allocator, exactly like a CFlat `new` there, and a pointer allocated by
either language can be released by the other. Two families are the interim state, not the design.

## Today

- CFlat `operator new(i64)` / `operator delete(void*)` and the aligned pair live in `cflat/core/memory.cb`. They
  dispatch through `thread_local IAllocator __active_allocator` (null = malloc/free), then the `__try_owner_dealloc`
  hook (cross-program frees) and the HeapAudit hooks. `program` installs a per-thread BlockAllocator
  (`cflat/core/program.cb`) and frees it at program exit.
- C++ code calls the replaceable global allocation functions (`_Znwm`, `_ZdlPv`, ... on Itanium; `??2@YAPEAX_K@Z`,
  `??3@YAXPEAX@Z`, ... on MSVC), which resolve to libc++ / the MSVC CRT and go straight to malloc. A C++ allocation on
  a program thread bypasses the program allocator.
- C++ CLASS types already pair: CFlat `new C()` / `delete` of a C++ class call its operator new / delete (class
  operator pairing, 767ae268).
- Measured mismatch (WINRUN, box 7): C++ `new int` freed by CFlat `delete` works in the -o exe but corrupts the heap
  under Windows --run; CFlat `new int` freed by C++ `delete` corrupts the heap in the Windows exe too. Root cause of
  the exe case not yet known (two CRT heaps in one image is the first suspect).

## Direction

C++ allows a program to REPLACE the global allocation functions ([new.delete]; one definition per program, it beats
the library's). When the image contains C++ code, the compiler emits the replaceable family - plain, array, sized,
aligned, nothrow, aligned-nothrow, in the target's mangling - forwarding to CFlat's `operator new` / `operator delete`
and the aligned pair. C++ semantics kept: failure calls the new_handler loop then throws `std::bad_alloc` (nothrow
forms return null), size 0 returns a unique non-null pointer, sized delete ignores the size unless the allocator uses
it. CFlat's own `new`/`delete` do not change.

## Phases

0. Spike (no code kept). Matrix per platform (macOS -o, macOS --run, Windows -o, Windows --run): which family each
   C++ allocation site binds to; whether a main-image replacement wins for allocations made INSIDE libc++.dylib on
   macOS (weak-def coalescing) and inside the /MT CRT on Windows; why CFlat `new` -> C++ `delete` corrupts the Windows
   exe. Exit: the matrix in this file and a yes/no on "a replacement reaches library-internal allocations".
1. Replacement definitions, no `program` yet. Emit the family when a C++ TU or header import is present; forward to
   memory.cb. Cross-language scalar `new`/`delete` in both directions becomes legal: legs in test_cpp_interop flip
   from documented-error to value legs; the LANGUAGE.md "two families" line becomes "one family". Windows --run stops
   binding the operator new family to the host static runtime (WINRUN2) and binds the image's definitions.
2. Free routing (NEEDS RULING before build). Once both languages share the dispatch, a pointer can be freed under a
   different active allocator than the one that allocated it: C++ static-init memory (malloc, before any program)
   freed on a program thread, a C++-internal cache filled on one program thread and freed on another. Today only
   `__try_owner_dealloc` covers cross-program frees. Candidates: an allocator `owns(ptr)` query by address range
   (BlockAllocator knows its blocks), or an allocator-side header (NOT ruled in - see the 2026-09-04 note in
   delete-form-static-analysis.md). Pick one with the maintainer.
3. `program` teardown vs escaped C++ allocations (NEEDS RULING before build). The program allocator is freed at program
   exit, but C++ can park program-thread allocations in storage that outlives the program: function-local statics,
   libc++ locale / iostream caches, a C++ global container. Candidates: route allocations made under a C++ static-init
   guard (`__cxa_guard_acquire` window, MSVC `_Init_thread_header`) to malloc; or keep the allocator's blocks alive
   while any C++ static holds them; or refuse/document C++ statics initialized on a program thread. CFlat's own globals
   follow the Rust ruling (no exit-time destruction), which does not settle this.
4. Gates and measurement. A `program` leg (extend Test/test_allocators.cb or test_cpp_interop, no new test file) that
   runs `std::vector<int>` / `std::string` work inside a program body and proves the program allocator served it (its
   stats or HeapAudit) with no leak and no double free at exit; std_full tier-2 and test_libs parity unchanged (the
   extra dispatch must not move the C++ ratios); HeapAudit must not report libc++ static-lifetime allocations as leaks.
   Windows test.bat + macOS test.sh green.

## Out of scope

- `new T[n]` / `delete[]` across languages for non-trivially-destructible T: the C++ array cookie and CFlat's hidden
  count (`move T*` slot) differ. Stays a documented error unless phase 0 shows the layouts already agree.
- `malloc` / `free` mixed with `new` / `delete` in either language - undefined, as in C++.
- `--run` with `program` (refused: --run is single-threaded).
