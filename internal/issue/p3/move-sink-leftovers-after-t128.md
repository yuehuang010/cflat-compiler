# Move-sink leftovers after T128 (ternary / `??` / indirect sinks)

PRE-EXISTING on master (T128 reviews 1-2, 2026-10-11). The review probes went with the worktree;
items 1-3 landed in T227 and were removed; each remaining item is its own repro (P = struct with a destructor, mv takes `move P*`).

4. Reassigning a field after it was consumed (`mv(s.p); s.p = new P(); s.p->v`) is still
   "use of moved variable" - direct and ternary forms agree; a field re-init after a consume should
   clear the moved mark like a local does.
5. Cosmetic: an unnamed function<> move parameter prints as `'0'` in the borrowed-sink text.
6. (T227 review, pre-existing) a raw copy of an owning `move` PARAMETER into a sink is not refused and
   double frees (`int f(move P* n) { P* view = n; return mv(view); }`, rc 134); `P* a = o; t.p = a ?? s.p`
   with `o` an owning local double frees.
7. (T227 review, wording) after `P* m = move n; mv(view)` the refusal names `n` as the owner (m owns it).
