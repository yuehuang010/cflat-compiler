# Moved-from struct leftovers after T490

T490 (2026-10-24) made a moved-from plain CFlat struct (user dtor, no owning fields) dead at scope exit. Still open:

- (master too) Re-assigning a moved-from struct after a straight-line move runs the dtor on the dead value:
  `P a; take(move a); a = mk(3);` -> an extra dtor run (probe dtor trace 614, expected 64); `a = move b; b = mk(2);` same.
  Branch/loop moves (drop flag present) are already correct. Fix: in the struct-assignment old-value destroy
  (MainListener_Expressions.cpp ~7360-8300) skip the dtor when the local is moved, has no drop flag, and
  ShouldSkipMovedFromStructDestructor passes (mirror LLVMBackend_OwnershipTemps.cpp ~6796), or give eligible locals a drop flag.
- (T418, master too) A by-value C++ class parameter consumed with `_ = move t` runs its dtor 3 times for 2 objects
  (`cfm(rettw())` -> stats 10130, clang 10120). The T490 r1 attempt (skip the release for parameters) fixed the count but
  deferred the release to function exit (stats inside the callee 10001 vs master 10010), unlike `_ = move` on a C++ local;
  backed out. Fix: keep the immediate release and disarm the parameter's later cleanup.
- (master too) `take(move b.inn)` (moving a struct field out) destroys `inn` twice.
- (master too) `Bx b = { inn = move a };` runs one extra dtor on an empty default value.
- Scope: structs with a raw-pointer or `int[]` view field are excluded from T490 (cautious; C-style `~Buf(){ free(data); }`)
  and still get 2 dtor runs after a move. Ruling question: does "no owning fields" cover raw pointers?
Repros: scratch/repro_keep/t490/ (main checkout).
Found: T490 r1 review (2026-10-24).
