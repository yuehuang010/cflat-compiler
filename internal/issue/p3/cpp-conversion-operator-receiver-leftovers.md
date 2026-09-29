# C++ conversion operator receiver: leftovers after A9b

Summary: these are the P3 leftovers from the A9b reviews (2026-09-28). A9b runs `operator bool` / `operator T` on the object's storage, recovered from its load, or from a select/phi of loads, nested up to depth 8.
- `f ? mkh(1).b : mkh(2).b`: an rvalue-member arm still gets a bitwise copy as the receiver (the `bad=1` column in scratch/a9b2/pd.cb). Master is the same.
- A `const Z&` result with both a const and a non-const `operator bool` picks the non-const one; clang picks const. This is the const-unenforced ruling, noted only because the receiver is now the real object.
- `long x = p` with both `operator bool` and `operator long` is refused as ambiguous; clang picks `operator long`. Master is the same.
- `W w = p` / `(W)p` with a class-typed `operator W` is refused with "cannot cast an aggregate value". Master is the same.
