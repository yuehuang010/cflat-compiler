# Unique-field consume: remaining gaps after T230

Filed 2026-10-15 from T227 (PRE-EXISTING on master). P = struct with a destructor, S/T hold `unique P* p`.
T230 fixed the plain form: `t.p = s.p` / `t.p = move s.p` mark `s.p` moved (later deref or member
read refused "use of moved variable 's.p'", `s.p == nullptr` still compiles, `s.p = new P()` and a
self-assign `a.p = a.p` / `a.p = move a.p` leave it live). Same for `ps->p` through a local
pointer and for unique locals (`b = a; a->v` refused; re-init after `move a` clears the mark).
Legs: Test/errors/err_unique_field_consume_read.cb, Test/test_move.cb `unique_local_reinit_*`,
`unique_field_consume_*`, `unique_array_field_consume_*`, `unique_field_self_move_*`.

Open:
2. Unchecked consume paths (r3 null-state keying, LLVMBackend::UniqueFieldNullKey): only a path
   rooted in a plain struct value LOCAL is tracked. A path through a pointer (`ps->p`, `this->p`),
   or rooted in a parameter, alias local, static local or global, and any element path (`a[0].p`)
   is not marked, because another name may re-initialize it (false refusals in review 1). Fix
   direction: points-to for field roots; `this->p` needs the this-arg binding.
## Ruling 2026-10-15 (T230 leg `unique_field_coalesce_assign_other_alive`): both `??` arms are maybe-moved
`c.item = a.item ?? b.item;` marks BOTH arms maybe-moved, like a conditional local move (Rust semantics): a later
`b.item->id` (or `a.item->id`) is refused until the field is reassigned. The read-only leg at Test/test_move.cb ~1725
that pins `b.item` readable is superseded by this ruling - convert it to an err_ leg (the one sanctioned edit of it).
