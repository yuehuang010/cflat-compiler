# A global named like a function of an EXPLICITLY imported core .cb file is accepted

Found 2026-10-15 by the T268 review (repro scratch/repro_keep/t268_rev/rev_t268_opus_coreimport.cb). Pre-existing acceptance
(master too); T268's same-scope var/function redefinition rule exempts every core library function
(FunctionSymbol::IsCoreLibraryFunction), not only the names runtime.cb pulls in implicitly.

`import "math.cb"; double tan = 2.0;` compiles; clang refuses the `<cmath>` twin. A later `tan(0.5)` then fails with a
misleading "no overload" error. Fix direction: an explicit `import` of a core file makes its functions imported symbols for
the redefinition rule (narrow the exemption to the implicit runtime.cb graph), or a maintainer ruling that explicit core
imports count as implicit. Check core files imported by both runtime.cb and the user.
