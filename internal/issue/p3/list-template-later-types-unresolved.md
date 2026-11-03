# List template fails for size_t and later-declared struct types

Summary: List instantiations cannot resolve `size_t` or a struct declared later, even outside tuples.
Repro: `list<size_t>` and `list<LaterDeclaredStruct>`.
Master vs expected: master fails type resolution; expected both element types to resolve. Probe: `scratch/repro_keep/box1022/t433/rev_t433_opus_probe.sh`.
Found: T433 r2 opus review, 2026-10-24 box, PRE-EXISTING

Root cause (MIX8): not reproduced as a list defect for `size_t`: `size_t` is not a CFlat type name (`size_t x;`
fails in plain code too; `using size_t = u64;` then `list<size_t>` passes; the native alias is `win_usize`).
`list<Later>*` and `new` after the struct definition pass. The failing shape is a by-value `list<Later>` field
declared BEFORE `struct Later`: ProcessPendingInstantiations instantiates list<T>.grow (core/list.cb:48,
`sizeof(T)`) at the enclosing declaration's flush while Later is still undefined ("sizeof/alignof: type 'Later'
is incomplete", reported at a list.cb location). Pointer-only uses already defer (PendingWaitsOnStructBody).
Fix direction: defer method-body instantiation of a generic class instance whose type argument is scanned but
undefined (needs a ruling: destructor of the enclosing holder depends on those methods).
