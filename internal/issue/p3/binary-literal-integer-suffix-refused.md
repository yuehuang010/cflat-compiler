# Binary integer literals refuse integer suffixes (`0b1010u`, `0b1ULL`)

Found by T76 review 2 (2026-10-09), pre-existing on master 608183756.

Summary: CFlat.g4 (~1583-1586) has no IntegerSuffix on the BinaryConstant alternative, so
`0b1u`, `0b1L` and `0b1ULL` are syntax errors. C23 and C++14 accept integer suffixes on binary
literals. The T76 ladder (IntegerLiteralLadderPick) already types suffixed literals for every
base, so only the lexer rule is missing.

Repro: scratch/repro_keep/t76/rev_t76_binsuffix.cb (+ .cpp oracle).

Fix direction: allow IntegerSuffix? after BinaryConstant in CFlat.g4. ParseNumberConstant's
binary path (base 2 since T76) must strip the suffix the same way it does for hex and octal.
Add legs next to the T76 binary legs in Test/test_basic.cb.
