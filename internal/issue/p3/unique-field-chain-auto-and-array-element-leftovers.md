# p3: unique field chain / consume leftovers - `auto` interface decl leaks, array-element consume leaves the source set

Pre-existing; found by the T376 review r2 (2026-10-21). Case bodies: scratch/repro_keep/box1021/t376_rev/rev_t376_opus_cases/ (r2f_*,
r2c_*; `.extra` holds the struct definitions).

1. `UI w = default; auto o = w.i = new BI();` (UI has `unique IB i`, BI implements IB): compiles, leaks one allocation, no destructor
   runs. The explicit `unique IB o = w.i = new BI();` is refused ("cannot initialize unique<IB> 'o' from borrowed value 'w'").
   Direction: `auto` deduction from a unique-interface chain must take the same path as the explicit type (refuse, or consume via T376's
   RHS-backed result signal).
2. `struct UA { unique PS*[2] q; }; UA a = default; unique PS* o = nullptr; a.q[0] = new PS(5); o = a.q[0];` - o owns the object but
   `a.q[0]` stays non-null (v=50 in the probe encoding). The declaration chain `unique PS* o = a.q[0] = new PS(5);` nulls it. Risk: a later
   use or destruction of a.q[0] double-frees - check ASan. Direction: the two-statement consume of a unique array-element field must null
   the element like the field form.
