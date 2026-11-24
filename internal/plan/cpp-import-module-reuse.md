# C++ imports: reach a dependency through its own module (cold memory peak)

Status: PLANNED (direction ruled 2026-11-08, confirmed 2026-11-18, maintainer). Not started; the mechanism is not
designed yet - phase 1 is the design spike. Issue: `internal/issue/p2/cold-cpp-interop-compile-peaks-23-27-gb.md`.

## Ruling

- 2026-11-08: each import is already a module (a PCH-like unit). When main.cb imports A and B and A itself
  imports B, the B module is loaded once and reused - never re-parsed or re-included into another group. No
  shared libc++ preamble / PCH across groups. Target: once a cold compile has finished parsing, memory comes back
  down to about the warm level (~3.5 GB then, ~1.5 GB measured 2026-11-16).
- 2026-11-18: no candidate-group skip. A group may not be dropped from a fan-out because its closure declares
  nothing the probe can see - a candidate's headers come first and can change a dependency template's lookup, so
  a skip can change a pick. Until this plan lands, the ~20 GB cold peak is accepted.

## Today (measured 2026-11-08, Mac Release, fresh cache, `-o`, peak footprint)

Cold `Test/test_cpp_interop.cb` peaks ~21.5 GB. 13.4 GB of it is prelude include parses: a request whose group
carries dependency headers includes them into the primary group's interpreter for good, and fan-out probes do
that to every group. `cpp_interop_basic.h` is included into 207 of 216 groups, ~62 MB each = 12.9 GB. Sources:
the free / infix function loop (LLVMBackend_CxxRequestFunctions.cpp) and the type / template fan-out
(LLVMBackend_CxxRequestTypes.cpp), `MakeCxxRequestGroup`, `CxxIncrementalGroupKey` keyed on ownerHeaders only.

## Already tried - do not retry

- Shared libc++ preamble / PCH per group: rejected by ruling 2026-11-08.
- Rehosting a request into the dependency's TU (memdedup): 21.0 -> 9.7 GB, but the primary's headers are then
  parsed after the host's, which flips lookup and preprocessing (master / clang 11, rehost 22). Rejected.
- Candidate-group skip + negative memo (fix/memfan): the cost moves to the next fan-out site, and a skip can
  change a pick. Rejected by ruling 2026-11-18.
- Resetting each request PTU's llvm::Module after harvest: no change in peak footprint.
- Landed and kept: interpreters released after LinkCxxCompanionModules + mi_collect; movedf sets as interned ids.

## Phases

1. Design spike (read-only, no landing). Find what "reached through its module" can mean inside the incremental
   executor: can one parsed dependency unit be IMPORTED into another group's interpreter (clang module / AST
   import of the already-parsed unit) with the group's own headers still first, so lookup order is unchanged?
   Deliverable: one mechanism with a pick-parity argument, measured on the basic.h fan-out, written into this
   file. If no mechanism keeps include order, say so and return to the maintainer - do not fall back to a skip.
2. Prototype on the free / infix function loop only; pick-parity run (program output, cold / warm clang parse
   counts, `--error-on-cpp-reparse`) on test_cpp_interop and test_cpp_interop_template.
3. Type / template fan-out and every other site with a dependency group - one rule for all sites, else the cost
   moves (measured with the fix/memfan prototype).
4. Post-parse plateau: memory after parsing returns to the warm level.

## Phase 1 spike results (2026-11-18, Mac, clang 23.1.0, memory only - host not perf-stable)

Harness: scratch/modspike/ (run.sh, run2.sh, repl.sh, delta.sh, stdmod.sh, lsv.sh). Footprint is
`/usr/bin/time -l` "peak memory footprint" of one clang / clang-repl process; a host header stands for group
k, `cpp_interop_basic.h` (4,258 lines, 15 includes) is the dependency B, the probe is
`s + v` on cppi::ConstPointerRankShift. clang-repl runs the same clang::Interpreter the groups use, and
it accepts `-fmodules -fmodule-map-file= -fmodule-file=`: a header module imported by `#include` executes
correctly (probe=5).

Measured (clang-repl, codegen + run):

| host k | k textual | k + B textual | k modular-std | k + B modular-std |
|---|---|---|---|---|
| cpp_t48_nullptr.hpp | 13 MB | 90 MB | 81 MB | 89 MB |
| cpp_interop_stream.h | 90 MB | 109 MB | 81 MB | 91 MB |
| cpp_t43_ref_ops.hpp, cpp_namespace_class_constants.hpp | 13 MB | 90 MB | 81 MB | 89 MB |

So B through a module costs +8-10 MB per group instead of +19-77 MB textual (basic.pcm is 2.5 MB with a
modular std, 19.8 MB with libc++ inside it). Most of the 81 MB floor is the 42 MB std.pcm, which clang reads
into a heap buffer per CompilerInstance; one in-process module cache shared by all groups would hold it once
(not measured - needs an in-process harness, not clang-repl).

What fails - every variant that keeps libc++ textual (no shared std module):
- k textual + B as a header module (libc++ inside B): `-fsyntax-only` passes, codegen fails - 17 errors
  "'std::basic_string_view<char>::basic_string_view' from module 'cppi_basic' is not present in definition
  of 'std::string_view' provided earlier". `-fmodules-local-submodule-visibility` does not fix it.
- k and B both header modules, libc++ textual inside each (the "each import is its own module" reading):
  driver `-fsyntax-only` passes (20 MB), codegen fails - 17 errors; with local submodule visibility 2 errors
  remain (std::logic_error / std::length_error ctors taking `const string&`), either import order;
  clang-repl segfaults on the same input.
- Conclusion: clang 23 does not reliably merge two textual copies of libc++ that live in different modules
  (or one module + the TU). It works only when libc++ itself is a module (SDK module map, 0 errors).

Consequences:
1. The only working mechanism is a modular standard library shared by every group's modules - the shared
   system closure the 2026-11-08 ruling excludes. It also has no Windows counterpart: the MSVC STL ships no
   clang header-module map, and Windows is the host that pages (27 GB).
2. Pick parity, if a modular std were allowed: a header module is built in isolation, so B no longer sees
   k's macros or k's namespace-scope declarations, and every group's libc++ becomes an import (config
   macros a user header defines before `#include <vector>` stop applying). B-in-isolation equals textual
   B-after-k only when no k-only macro or k-only namespace-scope name occurs among B's closure identifier
   tokens (and no k-only operator declaration meets an operator token in B); both sets come from the
   groups' own parses. When the check fails the request must keep today's textual path, which cannot be
   mixed with module imports in the same interpreter (the merge failure above), so it needs a separate
   fully textual group.
3. A non-module reading (ExternalASTSource / ASTImporter pulling B's decls from B's live interpreter, the
   lldb approach) keeps k textual, but merges libc++ by structural equivalence instead of ODR - expected to
   be weaker than the module merge that already fails; not prototyped.

Status: per the phase 1 rule, no mechanism that keeps include order and avoids a shared system closure was
found - back to the maintainer. Decision needed: allow a modular std (macOS/Linux libc++ only, Windows keeps
the ~27 GB path), or try the ASTImporter reading, or keep accepting the cold peak.

## Acceptance

Cold test_cpp_interop peak under ~8 GB (`/usr/bin/time -l` "peak memory footprint", not max RSS), program
output, cold / warm clang parse counts and `--error-on-cpp-reparse` unchanged, `scratch/libs_perf.sh` no slower.

## Open questions

- Phase 1 decides whether the unit is a clang module, a serialized AST, or the live interpreter's AST; the cache
  format changes with it (shipped cache format - maintainer sign-off before phase 2).
- Negative fan-out results are not memoized today (the same wrapper ran the 214-group loop twice); a memo is
  pick-neutral and could land on its own - not ruled.
