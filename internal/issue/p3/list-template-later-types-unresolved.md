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

RULED 2026-10-31 (main session, from precedent; maintainer confirmed 2026-10-31), same-file part: a by-value `list<Later>` field (or any generic
class instance whose type argument is scanned but not yet defined) declared BEFORE `struct Later` is LEGAL in one file - C++ accepts
`std::vector<Later> f;` on an incomplete element type and completes it at first member use, and CFlat's pointer uses already defer
(PendingWaitsOnStructBody). Method-body instantiation (and the holder's destructor, which calls them) is deferred until the argument
type is defined or the module scan ends; still undefined at module end = the existing incomplete-type error at the holder's line.
Test consequence: the T433 probe shape becomes an accept leg in Test/test_generics.cb. The CROSS-FILE part (imported file naming the
importer's later type) and the `struct Never;` syntax were RULED 2026-10-31 (maintainer): keep refusing, no `struct Never;` - see imported-union-later-holder-forward-reference.md.
