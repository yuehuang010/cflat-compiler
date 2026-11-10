# Paren-aggregate init moves a prvalue class argument into a member (clang elides it)

Found 2026-10-15 by T252. `struct Cnt { int v; Cnt(int); Cnt(const Cnt&); Cnt(Cnt&&); }; struct CM { Cnt c; int k = 9; };`
`CM l = CM(Cnt(5), 3);` (and `new CM(Cnt(5), 3)`) counts made=1 copies=0 moves=0 under clang++ -std=c++20 but made=1
moves=1 in CFlat. CFlat materializes `Cnt(5)` as a temporary and passes it to the constructor wrapper as an xvalue,
so the member is move-constructed. Same on master for the local form. Lvalue members (copy count) match clang.
Fix direction: a prvalue class argument to an aggregate or constructor wrapper must be constructed in the callee's
parameter or member slot (guaranteed elision), not materialized and moved. Needs a design pass over wrapper argument
passing; not a one-site change.


Root cause (fix box 2026-11-08, Luna + Haiku rounds, no code landed): RequestCxxVariadicConstructor
(LLVMBackend_CxxRequestTypes.cpp) emits `::new (p0) CM(static_cast<Cnt&&>(p1), ...)`, and `Cnt(5)` is built by its
own wrapper call first, so the member is always move-constructed. The only C++-valid elision is a wrapper that
spells the member's constructor arguments (`::new (p0) CM(Cnt(p1), p2)`, zero moves per the clang oracle), which
needs deferred argument evaluation at every caller (declaration, `new`, brace, nested, expression, ternary phi).
Repro: `scratch/repro_keep/parenagg/min.cb` (5 with clang, 6 in CFlat).
