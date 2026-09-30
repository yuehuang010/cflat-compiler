# Explicit specialization in a separately imported header gets the primary template's layout

Found 2026-09-29 by the C2 review (pre-existing on master). Silent miscompile.

Repro: header A `namespace n { template<class T> struct S { T v; }; }`, header B (includes A)
`namespace n { template<> struct S<long> { long pad; long v; }; }`, imported on SEPARATE lines
(`import cpp "a.h"; import cpp "b.h";`). CFlat lays out `n.S<long>` like the primary template and
writes `v` at offset 0 while C++ reads it at offset 8. A grouped import `import cpp { "a.h", "b.h" };`
gets it right. Probes: scratch/repro_keep/c2/c2rev/ (main checkout).

Root cause: not established. Likely the specialization request for `S<long>` is answered from the
first header's TU (primary template only) because the per-import request / layout cache is keyed on the
template, not on the TU that holds the explicit specialization.

Fix direction: a specialization request must be resolved in a TU that sees every imported header that
can declare an explicit/partial specialization of that template (or the specialization key must include
the header set); add a value leg with the separate-import shape.
