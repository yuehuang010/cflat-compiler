# Unique temporary bound to a raw local, or used as an interface receiver, leaks

Summary: an owning call-result temporary that is not passed to a parameter still leaks (no dtor, no free) in these shapes.
- Fixed in UQRAW: `S* q = mkU(1);` (`unique S* mkU(int)`) and `q = mkU(2);` on an existing raw local - now a compile error
  steering to a `unique` local or `.release()`. Accept-set kept: `S* r = mkU(1).release();`, `S* b = u.get();`, `&*u`,
  borrowed call results, `mkU(1)` passed straight to a sink or borrow parameter, `mkU(1)->v` / `mkU(1)->m()`, `return mkU(1)`.
- Fixed in UQRAW r2: method calls (`S* q = f.make(1);`, `q = f.make(2);`) via the resolved call-result value; owning `?:` arms
  (`k ? mkU(1) : b`, `k ? mkU(1) : nullptr`) per arm, decl and assign; interface receiver `mkI()->val()` refused ("bind it to a
  'unique' local first"). Legs: err_uqraw_direct_bind, err_uqraw_assign_owning_call, err_uqraw_iface_receiver.
- Open: a borrowed non-move parameter moved into a plain `T*` slot - see c-flexible-array-member-leftovers.md (maintainer ruling).
Repros: scratch/repro_keep/uqbind/ (direct_bind.cb, tern3.cb, ifacerecv.cb).
Found: UQTEMP r1 opus review, 2026-10-30 box 7, PRE-EXISTING (master = fix).
Re-verified on master after UQTEMP landed (4e9aad25): all three repros still leak (leaks=1).

UQTEMP r2 Opus review (2026-10-30), PRE-EXISTING; repros scratch/repro_keep/uqtemp2/:
- With `u` a named local unique, `return id(u.get())` / `return id(u)` are accepted while `return u.get()` is refused (B_named_L_get.cb,
  B_named_U.cb) - the frame-rooted gate does not follow a named-local owner through a call's borrow result.
- `return mkL(3).release();` in an `S*` function says "cannot return the heap array from 'new' as 'S*'" (misleading; A_release_direct.cb).

RULED 2026-10-31 (maintainer): REFUSE. An owning call result (`unique S*` / owning value / interface temporary) bound to a raw local by
declaration or assignment, in any arm of a join, is a compile error steering to a `unique` local or `.release()`; same for an owning
temporary used as an interface receiver (`mkI()->val()`) - bind it first. Refuse first, refine later; a leaking program stops
compiling, no implicit adoption into a raw local. Test consequence: direct_bind.cb, tern3.cb, ifacerecv.cb shapes become err_ legs
(the ternary one also falls under the 2026-09-03 mixed-join refusal); no accept leg flips. The r2 items (named-local owner through a
call's borrow result accepted; misleading release() text) are bugs under the frame-rooted gate, not rulings.
