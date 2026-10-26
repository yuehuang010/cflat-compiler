# Moved raw heap-array ternary arm destroys only one element

Summary: Adopting a moved raw heap-array ternary arm into a scalar unique destroys only one array element.
Repro: T443 repro described as a moved raw heap-array ternary arm adopted into a scalar unique.
Master vs expected: master destroys 1 of 3 elements; expected all 3 elements to be destroyed.
Found: T443 r2 sol review, 2026-10-24 box, PRE-EXISTING

Note 2026-10-24: not ternary-specific. `E* a = new E[3]; unique E* u = move a;` also destroys 1 of 3 on master
(repro scratch-free: struct E with a counting dtor). A raw `E*` carries no element count, so this waits on the
raw-array-count desugar ruling (block `new T[n]` -> bare T*), not on the ternary join.
