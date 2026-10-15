# Pointer to a unique<T> slot: field and argument typing gaps

Pre-existing on master (T199 Opus r1, 2026-10-14). A local `unique<Res>* p = &o;` is a non-owning borrow of the slot (like C++
`unique_ptr<T>*`), but:
- a struct FIELD declared `unique<Res>* p` is treated as a unique location: storing `&o` is refused "cannot store the address
  of a stack ... into unique field 'Hold.p'";
- `&o` on a `unique<Res>` local is typed `Res**` at a call argument, so `gx(unique<Res>* q)` finds no matching overload.
Fix direction: type `&o` as `unique<Res>*` (pointer to the library struct) everywhere, and treat a `unique<T>*` field as a plain
pointer field (the sugar `unique T*` is the owning spelling).
