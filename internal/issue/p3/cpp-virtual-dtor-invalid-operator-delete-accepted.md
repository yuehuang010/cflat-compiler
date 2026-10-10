# C++ virtual dtor with a deleted / private / ambiguous operator delete is accepted

Summary: a header whose class has a virtual destructor and an unusable class-scope operator delete
(deleted, private in a base, or ambiguous across bases) compiles and runs under cflat; clang++
-std=c++20 rejects it. Pre-existing on master; found by the T50-win review (2026-10-05).

Repro: scratch/repro_keep/t50win/{deleted,private_inherited,ambiguous}.{hpp,cb} and the *2 variants
(add a demanded class Q in the same chunk; run2.sh runs cold and warm). cflat prints 17 / 115.
Oracle: "attempt to use a deleted function" / "'operator delete' is a private member of
'probe::B'" / "member 'operator delete' found in multiple base classes of different types".

Root cause: LazyBodies::Skip skips the inline virtual dtor body, so Sema::CheckDestructor (which
diagnoses the lookup) never runs at parse time. Since T50-win, LazyBodies::ResolveUsedVTableDeletes
runs CheckDestructor for used vtables, but from HandleTranslationUnit - after IncrementalParser's
hasErrorOccurred check - so the diagnostic is swallowed and the dtor is only setInvalidDecl.

Fix direction: surface diagnostics raised by ResolveUsedVTableDeletes (relay as a clang error with
the "clang: " prefix), or run it before the error check. Cover unused vtables too only if clang
diagnoses them (it checks at the dtor body, so it does). Keep it lazy: no extra parse.
