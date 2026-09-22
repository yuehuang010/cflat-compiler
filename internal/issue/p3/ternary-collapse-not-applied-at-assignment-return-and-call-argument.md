Bucket: p3 (follow-up to the 2026-09-21 ternary ruling; declaration and alias forms are done)

# The ternary-collapse rule covers declarations and `alias` only - assignment, return and call arguments still use the old value join

RULED 2026-09-21: a ternary of lvalues collapses to the selected arm (`T x = c ? a : b;` means
`T x = a;` or `= b`). The fix (branch fix/ternary-collapse) implements it for `T x = c ? a : b;`
and `alias T x = c ? a : b;`. The remaining forms were measured and left alone because their
mechanism differs (scratch/tn2_matrix.md in that worktree):

| form | native owning struct | struct with C++ field / C++ class |
|------|----------------------|-----------------------------------|
| `x = c ? a : b;` | now emits (was rejected); dtor count measured 4 in the twin probe - verify balance | not measured |
| `return c ? a : b;` | rejected | rejected |
| `f(c ? a : b)` | accepted, by-value image of the selected arm ("rcall 0 2"), no collapse | accepted, no counter probe |

Fix direction: route each form through what the SAME form does for a plain lvalue (`x = a`,
`return a`, `f(a)`), selecting the arm by address first - the storage PHI the declaration fix
introduced. `f(c ? a : b)` for a C++ class must follow the by-value C++ parameter rule (copy-construct
into the indirect temporary). Acceptance: each ternary row of the matrix equals its plain-lvalue
row for the selected arm, both paths, counters balanced.
