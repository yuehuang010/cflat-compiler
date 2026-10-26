# By-value owning param leftovers after B13

Remaining after T412 remeasurement. The copy-classifier rows for generic methods, method-local
aliases, field-path writes, C++/interface receivers, read-only params, and direct internal-mutator
calls now pass on master. Item 13's local-array pointer alias is fixed in this worktree.

1. **S4 lambda whole-write leak (original item 1):** `c3d.cb` is absent from the supplied
   repro directory and the issue has no source body. The older master record is `c=3/d=2`;
   this run could not remeasure it. Restore the repro before deciding whether the leak remains.
2. **Literal-through-identity bond escape (original item 2):** `esc4.cb` is absent from the
   supplied repro directory and the issue has no source body. The older master record is rc=139;
   this run could not remeasure it. Out of scope for T412; needs separate bond/escape work.
3. **Conditional moved-source read (original item 3):** `e14.cb` is absent from the supplied
   repro directory and the issue has no source body. The older master record is rc=139; this run
   could not remeasure it. Out of scope for T412; needs path-sensitive moved-state design.
T196 setter bond escape remains out of scope. Master repro
`scratch/repro_keep/t196_rv1/rev_t196_opus_setter_bond_escape.cb`
returns 3 and ASan reports heap-use-after-free when a by-value Lambda param is stored and invoked
after its captured unique owner leaves scope.
