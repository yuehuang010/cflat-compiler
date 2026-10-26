# T515 residue: by-value returns remain refused

T515 item 1 is resolved in this worktree: a holder with a by-value field whose constructor stores a `this`-capturing closure now receives native sret construction. The holder can be initialized, used, and destroyed; copying the holder remains refused like copying the nested type.

T515 item 2 copy gaps are resolved: generic list members that copy closure-bearing elements are deferred until called, and round 3 refuses by-value local copies from alias/element accesses, including copies made from the borrowed range-for binding. A list can be declared, populated by move, read, iterated, counted, and destroyed; alias binding and in-place element calls remain accepted.

## Remaining gaps

- By-value returns of closure-bearing native values remain refused, for both fresh prvalues (`return S(4)`) and named locals (`return a`). They need a dedicated in-place return ABI path.

## Fix direction

Obtain the return-ABI ruling, then investigate sret-style returns for native closure-bearing results. Do not reuse the T345 r2b-r5 ABI attempt without resolving its ownership regressions.

Explicit moves such as `S b = move a` and `take(move a)` keep the closure pointing at moved-from storage by ruling. Preserve that behavior.
