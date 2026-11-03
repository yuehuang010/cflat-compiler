# &ref() return gate misses a global pointer that points at a local

Summary: with C++ `C& deref(C*& pp) { return *pp; }`, `C lc; gp = &lc; return &deref(gp);` (gp a global C*) is accepted - the
frame-rooted gate treats the global argument as safe, but the callee dereferences it to the local (dangling result).
Master: accepted (rc 0). The `??`/`?:` join form is refused since MIX9. Repro scratch/repro_keep/mix9/mix9_rev2/b2d.cb + rg2.h.
Fix direction: a constant/global argument is frame-safe only when its parameter cannot reach other storage (not T*&, not T**).
Found: MIX9 r2 opus review, 2026-10-30 box 7, PRE-EXISTING
