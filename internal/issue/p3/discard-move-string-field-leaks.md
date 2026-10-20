# `_ = move field;` on an owned string field leaks the buffer

Summary: inside a destructor, `if (a._ptr != nullptr) { _ = move a; }` on an owned `string` field nulls the field
but never frees the buffer. `{ string released = move a; }` frees it. Measured on master 87b36b2f1 (leaks --atExit:
999 leaks, 31968 bytes over 1000 iterations; 0 with the named-local form). `_ = move x;` is the canonical release
spelling, so this is a real leak, not a style choice.

Repro: struct with `string a` field, owned via `o->a = x.copy();`, destructor `{ _ = move a; }`, `delete o` in a loop.

Fix direction: the discard sink must run the string destructor on the moved-out value, as a named local's scope exit
does. Audit the other owning field types (unique<T>, list<T>) through the same `_ = move` path.
