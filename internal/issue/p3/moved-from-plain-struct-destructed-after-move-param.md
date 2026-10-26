# A plain struct moved into a `move` parameter is destructed twice (callee + caller scope exit)

Repro: scratch/repro_keep/box1022/e_plain_move_param.cb (raised by T417 review, 2026-10-22). A struct with a
user destructor and no owning fields, passed as `take(move a)` to `int take(move P p)`: the callee
destroys `p`, and `a` is destroyed again at its scope exit -> exit code 23 (2 destructor runs for one object).
C++ also destroys a moved-from object, so this may be intended; CFlat's ruling is that an explicit move
nulls the source and leaves it readable-as-null, which has no defined meaning for a plain value struct.

Needs a maintainer ruling before any fix: (a) moved-from plain struct is dead - skip its destructor (Rust),
or (b) keep C++ semantics (destructor runs on the moved-from value) and document it in doc/LANGUAGE.md.
Related: a by-value C++ class parameter consumed with `_ = move t` runs its destructor 3 times for 2 objects
(`cfm(rettw())` -> 2000029, T418 review) - that one is wrong under either reading.
