# `q = g ?? new T(x);` assignment with `g` null only at runtime leaks the fallback (p3)

Bucket: p3 (bounded leak, never an early free). The declaration shape `T* q = g ?? new T(x);` is
fixed: the RHS allocation has a hidden owner (LLVMBackend::NullCoalesceFallback) bound to the
declaration's scope, released at scope exit unless a copy of `q` escapes.

## Still leaks

- Assignment `q = g ?? new T(1);` to an existing binding. Not bound: the hidden owner would have to
  live in q's frame (not the statement's frame) and its slot must start null on every entry to that
  frame, which the binding site cannot guarantee for a conditional statement or a loop body.
- Escapes by design: a copy of `q` that is returned, stored in a global or field, or passed to a
  retaining callee keeps the fallback unreleased (the leak is the pre-fix behaviour, never an early
  free). Returning the join also gives the caller ownership (see the p1 issue on return adoption).

## Fix direction (assignment)

Bind the hidden owner at the assignment's store when the target is a local of the SAME frame,
initialise its slot null at the frame entry, and free the previous fallback before storing a new
one. Needs a design note first (frame re-entry in loops).

Also open (T220 review 1): `return g ?? new T(x);` leaks the fallback when g is null (same family). A p1 filed during
T220 ("caller adopts the borrowed arm") was disproved: its crash was the user's own `delete r` of a borrowed
pointer plus the owner's scope-exit free; `T* mk(T* g) { return g; }` + `delete r` crashes the same way. The real
residual is a p3 diagnostic gap: the "delete of a copy of an owner" rejection does not see through a call that
returns its argument.
