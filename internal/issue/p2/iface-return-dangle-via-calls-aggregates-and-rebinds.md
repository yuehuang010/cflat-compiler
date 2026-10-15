# Interface return can dangle through pointer-returning calls

From T91 reviews 1-2 (2026-10-10), pre-existing on master: a returned interface may box a pointer
borrowed from a local owner when the pointer passes through a call. The owner is freed at scope exit
while the returned interface still points at it.

## Repro

`scratch/repro_keep/t91/` contains the reviewer harness. Representative shapes are
`return idt(t);` where `idt` returns its parameter, `return u.get();` on a local `unique<Thing>`,
and a lambda capturing `t` called in the return.

## Fix direction

Add a callee-return provenance fact such as "returns parameter N", or use a borrow-checker style
rule that refuses these call results. T81's call rule currently treats a non-owning call result as
a borrow of something else. This item needs a separate design ruling.

## Also open: alias stored into a HEAP aggregate (T198 A5)

T198 (2026-10-14) refuses aliases stored into frame aggregates (struct field, fixed/dynamic array slot) and conditional
rebinds. Still compiling and dangling: `Thing* alias = owner; T198HeapHolder* h = new T198HeapHolder(); h->p = alias;
return h->p;` (master and after T198: dtor=1, garbage value). The heap slot has no frame root, so PointerValueRoot's
aggregate-slot tracing does not reach it; Luna stopped after 3 attempts. Same design family as the call-result item
(provenance through memory not rooted in the frame).

## Also open: indirect aggregate writes still dangle

- `N_store_via_pp.cb`: `Thing** pp = &h.p; *pp = alias; return h.p;` still dangles (master/r1/r2 exits 160/168/208).
- `N_ptr_arith.cb`: `s[1] = alias; Thing** b = &s[0]; return *(b + 1);` still dangles (master/r1/r2 exits 208/160/176).
- Escaped struct slot falls back to master's answer (T198 r2 F2 fix): any address use of the struct re-opens the dangle -
  a method call `h.p = alias; h.noop(); return h.p;`, `touch(&h)` read-only, `gh = &h` with no write, `&s[0]` next to an
  alias in `s[1]`. Common shape (methods on locals). Fix: a callee/escape summary that proves no write to the field.
- Struct copy / memcpy CARRYING an alias (`o.p = alias; h = o; return h.p;`) and a variable-index store
  `s[j] = alias; return s[0];` dangle (pre-existing).
- Conservative leftovers from T198: a variable-index overwrite loop (`s[1] = alias; for (j) s[j] = &g; return s[1];`) is
  refused (master accepts); the correlated-branch refinement (LLVMBackend_MoveDataflow.cpp ~354-366) records the then-block
  store before the branch condition, so `if (k==99) i=&loc; if (k==99) i=&g;` is still refused - fix or remove it.
