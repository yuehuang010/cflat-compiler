# By-value owning param leftovers after B13

Remaining after T412 remeasurement. The copy-classifier rows for generic methods, method-local
aliases, field-path writes, C++/interface receivers, read-only params, and direct internal-mutator
calls now pass on master. Item 13's local-array pointer alias is fixed in this worktree.

1. **S4 lambda whole-write leak (original item 1):** `c3d.cb` is no longer present under
   `/Users/felixhuang/source`; the older master record is `c=3/d=2`. Restore the repro before
   deciding whether the leak remains.
2. **Literal-through-identity bond escape (original item 2):** `esc4.cb` is absent. The older
   master record is rc=139. Out of scope for T412; needs separate bond/escape work.
3. **Conditional moved-source read (original item 3):** `e14.cb` is absent; the older master record
   is rc=139. Out of scope for T412; needs path-sensitive moved-state design.
4. **Alias-return operator materialization (original item 7):**
   Repro: a struct with `unique<int> value`, an `alias T412AliasValue t412Alias()` returning a
   global, and `T412AliasValue result = t412Alias() + right;`. The alias-returned noncopyable
   value is refused with `cannot initialize value of type 'T412AliasValue' with a pointer of type
   'T412AliasValue*'`. This is separate expression materialization work.
5. **Lambda-local pointer assignment (original item 12, V17 x2):**
   `/Users/felixhuang/source/cflat-compiler/scratch/repro_keep/v17/rv17/x.cb` selector 2 writes
   through `q` after a lambda assigns `q = &b.xs[0]`. Current master exits 139; ASan reports a
   null-pointer write in `_x2`, before a caller-buffer result can be checked. Separate
   closure-capture investigation is needed.
6. **Unqualified generic method call (original item 13):**
   Repro: `void inner<T>(T value) { }` and `void outer<T>(T value) { inner<T>(value); }` in the
   same generic class. Bare `inner<T>(value)` fails with `unknown generic function 'inner'`;
   `this->inner<T>(value)` works. Not included in T412: the rejection happens in generic
   primary-name handling and dispatch later needs current-owner method lookup, so it is not a
   one-site fix.

T196 setter bond escape remains out of scope. Master repro
`/Users/felixhuang/source/cflat-compiler/scratch/repro_keep/t196_rv1/rev_t196_opus_setter_bond_escape.cb`
returns 3 and ASan reports heap-use-after-free when a by-value Lambda param is stored and invoked
after its captured unique owner leaves scope.
