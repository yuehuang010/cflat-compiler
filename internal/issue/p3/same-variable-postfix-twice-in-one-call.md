# Same variable's postfix used twice in one call passes the same value to both

T67 review 1 (2026-10-07); master same. `two(y++, y++)` (native) and C++ `sum2(y++, y++)` both get
0, 0 and y ends at 1; clang gives an implementation-defined order with both increments applied
(and -Wunsequenced). Native brace lists behave the same. Unsequenced in C/C++, so the C++ value is
not fixed, but losing one increment is wrong: y must end at 2. Decide: diagnose (clang warns) or
sequence left to right like brace lists. Probes: scratch/repro_keep/t67_rev/.

T71 review 2 (2026-10-07, on 393f0bd9): `two(y++, y++)` now leaves y at 2 (both steps applied); the
open part is the argument values. Named-field brace init has the same shape: `S z = {f = x++, g = x++}`
(int) gives z.g 10 where C++ gives 11 (master same). Probes: scratch/repro_keep/t71_rev2/.
