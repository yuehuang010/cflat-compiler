# Non-incremental C++ request mode fails a std::map lookup the incremental mode passes

**Summary.** With the incremental request executor disabled (the unified reparse switch off,
`CFLAT_CPP_INCREMENTAL` unset / non-incremental path), the template fixture leg that looks up
through `std::map` is refused, while the default incremental executor binds it. Seen on
2026-09-26 while comparing modes for the trivial-destructor Debug assert; the assert itself is
fixed and the incremental path is the tested one.

**Repro.** Run `Test\test_cpp_interop_template.cb` with the non-incremental switch and compare
the first stop against the default run.

**Root cause.** Not established. Candidate: the non-incremental request TU does not see the
Sema-side completion of the map's node/iterator specializations that the live Interpreter
keeps across chunks, so the member harvest refuses the lookup member.

**Fix direction.** Decide whether the non-incremental path stays supported. If yes, diff the
`-v` member harvest of `std::map<...>::find` between the two modes and align the request-TU
completion with the live-Sema one; if no, remove the switch.
