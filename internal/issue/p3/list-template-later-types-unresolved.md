# List template fails for size_t and later-declared struct types

Summary: List instantiations cannot resolve `size_t` or a struct declared later, even outside tuples.
Repro: `list<size_t>` and `list<LaterDeclaredStruct>`.
Master vs expected: master fails type resolution; expected both element types to resolve. Probe: `scratch/repro_keep/box1022/t433/rev_t433_opus_probe.sh`.
Found: T433 r2 opus review, 2026-10-24 box, PRE-EXISTING
