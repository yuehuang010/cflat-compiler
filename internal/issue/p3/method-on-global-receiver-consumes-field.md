# p3: implicit consume of a this-field through a local alias of `this` is not refused

**RULED 2026-10-21:** Option B: an implicit consume of a this-field (`o = p;` in a method) is refused in EVERY method; `move p`
required (Rust-like). See fix-issue-lessons.md digest.

T447 (2026-10-23) landed the ruling: assignment, declaration, owning return, move-sink argument (incl. ternary arms), brace/field
init and receiver-array elements (`p[0] = p[1]`; self-assignment exempt only for the identical slot) are refused without `move`.
Error legs: Test/errors/err_this_field_implicit_consume.cb.

Known gap (T447 Sol r1): receiver provenance is not carried through a local pointer alias of `this`:
`B* self = this; sink(self->p);` empties the field without `move` (accepted, master and T447). Raw borrowed-pointer provenance is
outside the function-boundary safety model, so this stays open as a p3 gap.
Repro: scratch/repro_keep/box1022/t447/alias_receiver.cb.
