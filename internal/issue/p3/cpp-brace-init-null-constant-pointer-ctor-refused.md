# Brace initialization refuses a literal 0 where clang picks the pointer constructor

From T87 review 1 (2026-10-10), pre-existing on master. With (short, int) / (int*, double) constructors,
`C c = {0, 1.5};` / `C{0, 1.5}` is refused by CFlat; clang++ -std=c++20 picks the pointer constructor
(returns 99). T87 fixed the parenthesized / new forms only; the brace selector (separate from the
scalar-reference mirror) does not spell a null constant as its literal.

## Repro
scratch/repro_keep/t87/rev_t87_brace.*

## Fix direction
Apply the T87 null-literal spelling (LLVMBackend_CInterop.cpp ~15375) to the brace selector request
(RequestCxxBraceConstructor) as well.
