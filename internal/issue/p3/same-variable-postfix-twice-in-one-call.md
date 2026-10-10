# Same variable's postfix used twice in one call passes the same value to both

T67 review 1 (2026-10-07); master same. `two(y++, y++)` (native) and C++ `sum2(y++, y++)` both get
0, 0 and y ends at 1; clang gives an implementation-defined order with both increments applied
(and -Wunsequenced). Native brace lists behave the same. Unsequenced in C/C++, so the C++ value is
not fixed, but losing one increment is wrong: y must end at 2. Decide: diagnose (clang warns) or
sequence left to right like brace lists. Probes: scratch/repro_keep/t67_rev/.
