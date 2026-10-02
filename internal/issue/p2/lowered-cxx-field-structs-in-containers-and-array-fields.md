# CFlat structs with non-relocatable C++ fields: dictionary slots, list.take leak, fixed-array fields

Summary: the V1b review (2026-10-01) found these. All three also fail on master. They are in plan scope (internal/plan/cflat-struct-nontrivial-cxx-fields.md) and were left out of V1b.
Probes and clang++ oracles are in scratch/repro_keep/v1b_rv4/ (runner run.sh).

- D3: `dictionary<int, S>` segfaults on the first `set` (e1.cb). `operator=` runs on calloc'd storage that was never constructed.
  Fix direction: use `construct_at` for fresh slots, at cflat/core/dictionary.cb:106, :194 and :257.
- D4: `list.take()` leaks one object, leaving live=1 (d3.cb). This happens even with a plain `list<cpptw.Twin>` (h1.cb).
  At cflat/core/list.cb:175, `T v = move _data[index];` leaves the moved-from element alive, then `construct_at` overwrites it.
  Fix direction: a declaration move from an element access destroys the source (as EmitLoweredValueIntoSlot does), or list.cb releases the slot.
- D5: a fixed-array field of a lowered struct (`struct A2 { S[2] arr = default; }`) is never constructed, so the first use crashes (b1.cb, b5.cb, a6.cb).
  Fixed-array locals (`S[2] x;`) work (b4.cb).

Crashes and leaks, hence p2.

V1b r5 review (2026-10-01, V1b landed 52a77168). These also crash on master; probes are in scratch/repro_keep/v1b/rv5/ (run.sh is the branch, runm.sh is master):
- R1: `_ = move o.s; o.s = b;` on a lowered field (f1.cb, f1b.cb).
  The field is zeroed without running its destructor (leak). Scope end then destroys zeroed bytes, and the assignment runs operator= on a zeroed std::list (rc 139).
  Fix direction: the discard path in MainListener_Expressions.cpp (~1782-1840) materializes the move like `S x = move o.s` (f1c.cb works) and destroys the temp.
- R2: a release carried across a loop back edge, as in `for (...) { a = b; _ = move a; }` (f5.cb).
  The second iteration's assignment hits the destroyed `a` (rc 139). The foreign C++ class path has the same flaw (h5.cb).
  Fix direction: a release inside the loop body gives the variable a conditional drop flag at the loop head.
- R3: `_ = move arr[0]; arr[0] = b;` (f2.cb; the foreign class path is the same, h2.cb). The element state is not known statically.
- Note: `*p = b` after a release through a pointer is untracked (f3b.cb). This falls under the borrowed-pointer ruling and is not a bug.
- Nit: a user-declared `operator=(const DA&) = default;` that is trivial is refused (d2_DA). Look up the copy-assignment decl and test `isTrivial() && !isDeleted()`, not `hasSimpleCopyAssignment()`.
- Nit: GetOrCreateMemberwiseCopy adds a %source slot and re-copies trivial fields for plain structs. The behaviour is the same, but the IR is not identical to before.

RULED 2026-10-01 (maintainer): Test/test_move.cb recv_temp_snapshot: the asserted leak count MAY go down when the V1b machinery removes that leak (the leg encodes a known leak, not desired behaviour).

RULED 2026-10-01 (maintainer): V1b assignment into a CFlat struct with a user-written destructor keeps DESTROY + REBUILD (CFlat `=` runs the old value's destructor); member-wise C++ operator= stays for structs without a user destructor.

## Q5 leftover (Opus r2 review, 2026-10-01; master is worse, not a regression)
Returning a by-value PARAMETER of a struct with a user destructor and a copy-only C++ field still runs the user destructor on the moved-from parameter: `return w;` / `return (w);` users=2 (plain-struct control 1), `return move w;` 3 (plain 2). MainListener_Statements.cpp finishReturnedLocal(..., parameter=true) skips the shell substitution. Repros scratch/repro_keep/q5_rv2/{lv,params_trace}.cb with _plain controls.
Pre-existing (plain struct too): `W shadow(W w){ {W w; return move w;} }` never destroys the caller's argument temporary (users=1, expected 2).
