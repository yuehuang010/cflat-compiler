# Owning struct through a borrowed pointer: `return *o` double-free (rc 133) - remaining: return *this refusal (RULED 2026-10-01)

Summary: these were found by the A10 review (2026-09-28). All are on master. Probes are in scratch/repro_keep/a10rv/.
- `Owned rt(Owned* o) { return *o; }` compiles and bit-copies without consuming (x=1 w=1, rc 133). The siblings disagree:
  - `return h->f` is refused (move required);
  - `Owned t = *o;` silently moves (w=-1);
  - `return move *o;` works.
  Needs a ruling: refuse like `h->f` (likely), or move like `T t = *o`. Site: ReturnSourceIsIndirectOwningLvalue
  (MainListener_Declarations.cpp ~9797) admits GEP storage only.
The by-value param bullets landed in 059d1f42 (B13) and the moved compound read in A10; leftovers are in p2/byvalue-param-owning-leftovers-after-b13.md.
Silent memory corruption, hence p2.

RULING 2026-09-30 (maintainer): REFUSE `return *o` through a borrowed pointer, like `return h->f`; the error suggests `return move *o;` or `return o->copy();`. Inlining and RVO do not change legality. The sibling `Owned t = *o;` (silently moves today) is not covered - measure before ruling.

V12 LANDED 19018a0c (2026-10-01): `return *o`, `return *h->q`, `return a[0]`
through a borrowed pointer (and generic T* with an owning T) are refused, suggesting `return move ...;`.
A plain `return *o` through an owning `move T*` parameter is accepted as an implicit move of the
dying pointee (d=1, no double free). Measured (not changed): `Owned t = *o;` moves and nulls the
source, no double free or leak in the probed shapes (scratch/repro_keep/v12/a10rv/r3-*).
RULING QUESTION: `return *this` in a method of an owning struct now copies via copy() when copyable
and MOVES OUT of the receiver when not (master: bitwise duplicate, rc 133). Should a non-copyable
`return *this` be refused like `return *o` instead? Probe scratch/repro_keep/v12/rv12/this.cb.

RULED 2026-10-01 (maintainer): non-copyable `return *this` in a method of an owning struct is REFUSED like `return *o` (the receiver is a borrow); the diagnostic suggests `return move *this;` or copy(). Copyable types keep the copy() path.
