# Hex floating literals lower to the wrong value

Found by T76 review 3 (2026-10-09), pre-existing on master 608183756.

Summary: a hexadecimal floating literal (`0x1p-2f`, `0x1.fp0`, `0x1.8p3`) is not lowered as a
float. ParseNumberConstant cuts the numeric part at the first non-hex-digit character
(cflat/MainListener_Utilities.cpp ~175-180: the hex scan stops at '.' / 'p'), so the literal is
read as the integer before the '.' or 'p'. clang (C17 / C++17 and later) accepts hex floats.

Repro (scratch/repro_keep/t76/rev_t76_r3b_f1.cb, f2.cb with rev_t76_r3_float.hpp; f0/f3 = template calls):
    printf("%.9g\n", (double)0x1p-2f);    // clang 0.25, CFlat 1
    printf("%.9g\n", (double)0x1.fp0);    // clang 1.9375, CFlat 1
    r76f.deduce(0x1p-2)                   // C++ template call: CFlat refuses, clang deduces double

Fix direction: when a hex literal has a binary exponent ('p'/'P'), parse it with strtod (which
accepts hex floats) and type it double, or float with an 'f' suffix. T76 already treats 'f' as a
hex digit unless a 'p' exponent is present, so the suffix rule exists. Value legs go in the
Test/test_basic.cb literal section. Is a hex float without 'p' an error, as in C?
