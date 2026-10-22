# p3: `auto` unique-interface chain leaks

Pre-existing; found by the T376 review r2 (2026-10-21). Case body: scratch/repro_keep/box1021/t376_rev/rev_t376_opus_cases/r2f_iface_auto_decl.body (`.extra` holds the struct definitions).

1. `UI w = default; auto o = w.i = new BI();` (UI has `unique IB i`, BI implements IB): compiles, leaks one allocation, no destructor
   runs. The explicit `unique IB o = w.i = new BI();` is refused ("cannot initialize unique<IB> 'o' from borrowed value 'w'").
   Direction: `auto` deduction from a unique-interface chain must take the same path as the explicit type (refuse, or consume via T376's
   RHS-backed result signal).
