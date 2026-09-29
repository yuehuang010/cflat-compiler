# Owning struct through a borrowed pointer: `return *o` double-free (rc 133) - RULING needed

Summary: these were found by the A10 review (2026-09-28). All are on master. Probes are in scratch/repro_keep/a10rv/.
- `Owned rt(Owned* o) { return *o; }` compiles and bit-copies without consuming (x=1 w=1, rc 133). The siblings disagree:
  - `return h->f` is refused (move required);
  - `Owned t = *o;` silently moves (w=-1);
  - `return move *o;` works.
  Needs a ruling: refuse like `h->f` (likely), or move like `T t = *o`. Site: ReturnSourceIsIndirectOwningLvalue
  (MainListener_Declarations.cpp ~9797) admits GEP storage only.
The by-value param bullets landed in 059d1f42 (B13) and the moved compound read in A10; leftovers are in p2/byvalue-param-owning-leftovers-after-b13.md.
Silent memory corruption, hence p2.
