# Move-sink leftovers after T128 (ternary / `??` / indirect sinks)

PRE-EXISTING on master (T128 reviews 1-2, 2026-10-11). The review probes went with the worktree;
each item below is its own repro (P = struct with a destructor, mv takes `move P*`).

1. `g(b)` through a `function<int(move P*)>` variable with a borrowed `b`: crashes at run time
   (exit 133) instead of the direct-call refusal "cannot move borrowed parameter ...".
2. `P* view = n; mv(c ? view : view)` and the direct `mv(view)` where `view` borrows an owning
   local: crash (double free) on master and after T128; the borrowed-copy detection only knows
   parameter origins, not local owners.
3. `bw(a ?? s.p); s.p->v` (non-move borrow param, owning field RHS): segfault.
4. Reassigning a field after it was consumed (`mv(s.p); s.p = new P(); s.p->v`) is still
   "use of moved variable" - direct and ternary forms agree; a field re-init after a consume should
   clear the moved mark like a local does.
5. Cosmetic: an unnamed function<> move parameter prints as `'0'` in the borrowed-sink text.
