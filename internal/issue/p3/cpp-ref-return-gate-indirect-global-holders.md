# &ref() return gate misses indirect holders of a global pointer

Summary: after refglob (2026-11-01), a global passed to a C++ `T*&` / `T**` parameter makes `return &ref(...)` frame-rooted. Still
accepted on master and fix, same dangling result: a LOCAL copy of a global's address (`C** lpp = &gp; return &deref2(lpp);`), a
`C**` FUNCTION PARAMETER passed through, and a global receiver object with a `C**` field. Found: refglob Opus review 1, PRE-EXISTING.
Repros: scratch/repro_keep/refglob/rev_refglob_repro/. Fix direction: treat any pointer-to-pointer reaching argument as frame-rooted
unless its storage chain is proven global-only (EmitReturnExpression frame-rooted classification, cflat/MainListener_Statements.cpp).
Also noted (consistent with ruling polarity, not a bug): `&pick(gp, cell())` with pick(C*& p, C& o) returning o is now refused.
