# list<P>.add fails when list<unique<Res>> is instantiated in the same function

Summary: `list<P> lp; lp.add(p);` followed by `list<unique<Res>> lu; lu.add(...)` in one function
reports "no overload of 'add' matches the given arguments." at the FIRST add (line 4). Each list
alone compiles. Found by T90 review r2 (pre-existing on master 2026-10-10).

Repro: scratch/repro_keep/t90/pre_listmix.cb (`cflat pre_listmix.cb --check`).

Root cause: not investigated. Suspect the second instantiation (move-only element, add(move T))
replaces or shadows the overload set registered for the first instantiation's `add` during the
forward scan, so resolution of the earlier call sees only the unique<T> signature.

Fix direction: find where list<T>::add overloads are keyed (mangled per instantiation?) and confirm
the two instantiations get distinct entries; regression leg in Test/test_move.cb or a list test.
