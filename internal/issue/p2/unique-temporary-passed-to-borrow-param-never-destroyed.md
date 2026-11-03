# P2: unique temporary passed to a borrowed pointer parameter is never destroyed

Summary: an owning `unique S*` call result passed straight to a borrowed `S*` parameter is never freed - no dtor, no free (real leak).
Repro: scratch/repro_keep/uqtemp/uq_native.cb (plain CFlat), uq_plain.cb (C++ ledger order variant, header h3.h).
`useU(mkU(3))` with `unique S* mkU(int)` and `int useU(S*)`: master prints `dtors=1` (only the named `unique S* q` is destroyed);
expected 2 - the temporary dies at the end of the full expression, like a C++ prvalue bound to a parameter.
Root cause (unverified): the borrow-param route takes the raw pointer out of the owning temp and drops the temp from the
full-expression ledger instead of registering it for end-of-statement destruction.
Fix direction: register the owning temp on the full-expression ledger when it binds to a borrowed (non-move) pointer parameter;
check `move` sinks still take ownership (no double free) and nested/conditional argument shapes.
Found: mixcond3 round-3 Opus review, 2026-10-30 box 7, PRE-EXISTING (master and fix identical).
