# Unique temporary bound to a raw local, or used as an interface receiver, leaks

Summary: an owning call-result temporary that is not passed to a parameter still leaks (no dtor, no free) in two shapes.
- `S* q = mkU(1);` (`unique S* mkU(int)`) and `S* x = k ? mkL(1) : b;` - the owning temp is bound to a raw (borrowed) local and never
  destroyed. Destroying it at the end of the statement would leave q dangling, so the likely direction is a compile error ("bind the
  owning result to a `unique` local") rather than a silent end-of-statement drop. Needs a ruling on refuse vs implicit adopt.
- `mkI()->val()` - an interface-value temporary used as a receiver leaks.
Repros: scratch/repro_keep/uqbind/ (direct_bind.cb, tern3.cb, ifacerecv.cb).
Found: UQTEMP r1 opus review, 2026-10-30 box 7, PRE-EXISTING (master = fix).
Re-verified on master after UQTEMP landed (4e9aad25): all three repros still leak (leaks=1).

UQTEMP r2 Opus review (2026-10-30), PRE-EXISTING; repros scratch/repro_keep/uqtemp2/:
- With `u` a named local unique, `return id(u.get())` / `return id(u)` are accepted while `return u.get()` is refused (B_named_L_get.cb,
  B_named_U.cb) - the frame-rooted gate does not follow a named-local owner through a call's borrow result.
- `return mkL(3).release();` in an `S*` function says "cannot return the heap array from 'new' as 'S*'" (misleading; A_release_direct.cb).
