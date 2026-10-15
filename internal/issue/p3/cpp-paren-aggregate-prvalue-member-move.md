# Paren-aggregate init moves a prvalue class argument into a member (clang elides it)

Found 2026-10-15 by T252. `struct Cnt { int v; Cnt(int); Cnt(const Cnt&); Cnt(Cnt&&); }; struct CM { Cnt c; int k = 9; };`
`CM l = CM(Cnt(5), 3);` (and `new CM(Cnt(5), 3)`) counts made=1 copies=0 moves=0 under clang++ -std=c++20 but made=1
moves=1 in CFlat. CFlat materializes `Cnt(5)` as a temporary and passes it to the constructor wrapper as an xvalue,
so the member is move-constructed. Same on master for the local form. Lvalue members (copy count) match clang.
Fix direction: a prvalue class argument to an aggregate or constructor wrapper must be constructed in the callee's
parameter or member slot (guaranteed elision), not materialized and moved. Needs a design pass over wrapper argument
passing; not a one-site change.

- (T252 review 1, pre-existing) explicit `move` of a C++ aggregate lvalue into a ctor call binds the copy ctor:
  `new r.CM(move l)` / `r.CM l3 = r.CM(move l2)` with `CM { Cnt c; int k = 9; }` gives copies=1 moves=0
  (clang 0 / 1). Repro scratch/repro_keep/t252_rev/cases/c12.cb (header r.h).
