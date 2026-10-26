# Paren-aggregate init moves a prvalue class argument into a member (clang elides it)

Found 2026-10-15 by T252. `struct Cnt { int v; Cnt(int); Cnt(const Cnt&); Cnt(Cnt&&); }; struct CM { Cnt c; int k = 9; };`
`CM l = CM(Cnt(5), 3);` (and `new CM(Cnt(5), 3)`) counts made=1 copies=0 moves=0 under clang++ -std=c++20 but made=1
moves=1 in CFlat. CFlat materializes `Cnt(5)` as a temporary and passes it to the constructor wrapper as an xvalue,
so the member is move-constructed. Same on master for the local form. Lvalue members (copy count) match clang.
Fix direction: a prvalue class argument to an aggregate or constructor wrapper must be constructed in the callee's
parameter or member slot (guaranteed elision), not materialized and moved. Needs a design pass over wrapper argument
passing; not a one-site change.

