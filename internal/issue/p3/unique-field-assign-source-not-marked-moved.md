# Unique-field consume: residue after MOVESINK

Filed 2026-10-15 from T227; rewritten 2026-11-19 after MOVESINK. P = struct with a destructor, S holds
`unique P* p`, `mvP(move P* p)` is a move sink.

Landed: T230 tracks paths rooted in a struct value local; MOVESINK tracks every other root - pointer
(`ps->p`, `o->inner.p`), parameter, `this->p` and bare `p` in a method (key "this.p"), static local,
global, constant-index element (`a[0].p`) - for `t.p = X` and `t.p = move X`: a later dereference is
refused "dereference of moved variable". Such an indirect path's mark is cleared by any later call
(except intrinsics / read-only callees) and any store of a non-null pointer or aggregate, unless the
store targets a local slot whose address was not captured before it and that is not the path's root
(nulldf::InterleaveIndirectClobbers). The SetNull lands at the end of the consuming statement, so its
own drop calls do not clear it (a later postfix call in the same statement, and a for-clause or loop /
if condition end, flush or drop it first); a store destination that may alias the source drops it
(NoteIndirectConsumeDestination). Legs: Test/errors/err_unique_field_consume_read.cb (MOVESINK block),
Test/test_move.cb `ms_*`. The 2026-10-15 `??` ruling leg conversion was already done
(coalesceJoinRightArmThenRead).

Open (measured on the MOVESINK exe, all PRE-EXISTING on master):
1. Move-sink (movedf, "use of moved variable") false refusals through an indirect root: after
   `mvP(ps->p)` a re-init through an alias pointer (`q->p = new P()`), the root (`s.p = new P()`), a
   call (`reinit(ps)`, `reinit(&s)`), a method (`ps->reset()`), or a rebind (`ps = &s2`) still refuses
   `ps->p->v`; same for `mvP(s.p); reinit(&s); s.p->v`, and for elements after `move a[0].p` /
   `mvP(a[0].p)` followed by `a[i].p = ...` or `S* q = &a[0]; q->p = ...`. Repro:
   ```
   struct P { int v = 0; ~P() { } }; struct S { unique P* p = nullptr; }; void mvP(move P* p) { delete p; }
   int main() { S s = default; S* ps = &s; S* q = ps; ps->p = new P(); mvP(ps->p); q->p = new P();
                return ps->p->v; }   // refused "use of moved variable 'ps.p'", valid program
   ```
   Fix direction: give movedf field marks on non-local roots the same clobber rule nulldf now uses.
2. Implicit `t.p = g.p` / `t.p = s.p` from a global or static local is not refused with the q11
   "out of storage that outlives this function" diagnostic the sink form gets (`mvP(g.p)`); only a
   later deref is refused now.
3. Known conservative misses (no diagnostic, never a false refusal): a call between consume and use,
   including the scope-end destructor of a destination declared inside the branch / loop body; a
   consume whose destination may alias the source; a dynamic-index element source.
4. A same-statement re-init through an overloaded OPERATOR call (not a postfix call) after the
   consume is not counted: only postfix call expressions drop the pending mark (possible false
   refusal, not observed).
5. do-while condition consume, deref in the body on the next iteration is missed (run-time 139; the
   direct `s.p` form is refused). Same on master. MOVESINK review 2:
   `int i = 0; int r = 0; do { r += ps->p->v; i++; } while (i < 3 && (t.p = move ps->p) != nullptr);`
