# Tuple alias and tuple element types fail resolution

Summary: Tuple aliases, function-type tuple elements, pointer elements, and generic tuple arguments fail to resolve.
Repro: `using TP = (int,Foo)`; tuple function-type element; `(int,Foo*) a = default`; `f<T>((T,int))`.
Master vs expected: master reports missing or empty type names or incomplete tuple; expected each declared type to resolve. Probe: `scratch/repro_keep/box1022/t433/rev_t433_opus_probe.sh`.
Found: T433 r2 opus review, 2026-10-24 box, PRE-EXISTING
