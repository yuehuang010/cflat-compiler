# N-D array brace init with a short inner row flattens across rows (silent wrong code)

## Summary

A braced multi-dimensional array initializer whose inner row has fewer clauses than the row width is filled as if the
initializer were flat: later rows' values slide into the short row. C/C++ value-initialize the rest of the short row.
No diagnostic. Plain CFlat arrays, master 6f5ed3b2b (found by the REFRES r2 review, 2026-10-30).

## Repro

```cflat
extern int main()
{
    int[2][3] m = { {1}, {4, 5, 6} };
    return m[0][0] * 1 + m[0][1] * 10 + m[1][0] * 100;   // C: m[0] = {1,0,0}, m[1] = {4,5,6} -> 401 (exit 145)
}
```

CFlat runs with exit 129: m[0] = {1,4,5}, m[1][0] = 6. Also 3-D (`i64[2][2][2]`), struct element 2-D arrays, and
plain-aggregate 2-D arrays (review probe cases reg_cflat_s2d 105 vs clang 46, reg_cflat_i3d 25 vs 46, reg_plain2d_short
69 vs 68). The global form `int[2][3] gm = {{1,2},{4}};` fails to compile instead.
Repro: scratch/repro_keep/ndshort/nd.cb (main checkout); more in cflat-fix-refres scratch/rev_refres_opus/r2/repros/.

## Fix direction

Map each explicit brace row to its own row base (row ordinal * row width) at every depth and value-initialize the tail of
a short row; reject too many clauses per row. The REFRES fix does this for C++ reference aggregates only
(EmitPositionalFixedArrayIntoSlot); the general path should share that row mapping. Global arrays use the constant path.
