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

## Acceptance

Cold test_cpp_interop peak under ~8 GB (`/usr/bin/time -l` "peak memory footprint", not max RSS), program
output, cold / warm clang parse counts and `--error-on-cpp-reparse` unchanged, `scratch/libs_perf.sh` no slower.

## Open questions

- Phase 1 decides whether the unit is a clang module, a serialized AST, or the live interpreter's AST; the cache
  format changes with it (shipped cache format - maintainer sign-off before phase 2).
- Negative fan-out results are not memoized today (the same wrapper ran the 214-group loop twice); a memo is
  pick-neutral and could land on its own - not ruled.
