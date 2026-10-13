# Interface return still dangles through calls, aggregate-stored aliases and conditional rebinds

From T91 reviews 1-2 (2026-10-10), pre-existing on master (each is a use after free: the owning local is
freed at scope exit while the returned interface still points at it). T91 refuses the direct shapes
(alias of an owning local, interior / field address of it, interface local boxing an alias, `*pp`);
these remain:
1. Pointers returned by calls: `return idt(t);` (idt returns its param), `return u.get();` on a local
   unique<Thing>, a lambda capturing t called in the return. T81's call rule treats a non-owning call
   result as a borrow of something else.
2. Alias of the owner stored into an aggregate, then returned: `h.p = b; return h.p;` / `ps[1] = b;
   return ps[1];` (b an alias of owning t). The store of an ALIAS does not consume t.
3. Conditional rebind of an interface local: `IThing i = b; if (k) i = &g; return i;` with k == 0 -
   the check reads only the last binding. Master's frame-array twin has the same gap
   (`IThing i = &arr[1]; if (k) i = &g;`).

## Repro
scratch/repro_keep/t91/ (reviewer harness probes; run.sh shape in harness/)

## Fix direction
1 needs a callee-return provenance fact ("returns its parameter N") or a borrow-checker style rule
(Rust refuses all three); 2 = track alias stores into aggregate slots in PointerValueRoot
(MainListener_Expressions.cpp ~1229); 3 = merge InterfaceBoxAliasOfOwnedLocal / InterfaceBoxFrameStorage
over every binding of the interface local, not only the last.
