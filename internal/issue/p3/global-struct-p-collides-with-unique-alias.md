# A file-scope `struct P` declared after a unique<T> instantiation collides with unique<T>'s alias

PRE-EXISTING (found while writing T113 legs, confirmed by T113 Sol review 1). Test/test_list_ownership.cb
with `struct P { int v = 0; };` placed late in the file (after code that instantiates unique<T>) fails:
"cannot return this value: its type does not match the return type of function 'P'" - on master and
after T113. The colliding spelling is unique<T>'s `using P = T*` (cflat/core/unique.cb:14-15). The
same declarations placed right after the imports compile (T113's legs do that).

Fix direction: a member alias of a generic aggregate must not be visible as (or shadow / be shadowed
by) a file-scope type of the same name outside that aggregate's body; check alias registration scope
for generic instantiations vs later file-scope declarations. Repro: move the T113 declarations in
test_list_ownership.cb below the first unique<T> use.
