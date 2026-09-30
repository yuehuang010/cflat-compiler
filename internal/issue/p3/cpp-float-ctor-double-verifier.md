P3
# C++ float constructor called with double fails LLVM verification

## Repro

`scratch/rev2/p_C_d_new.cb` calls `new r2.C(x)` where `C(float)` and `x` is a
`double`. The same argument also fails in declaration and expression constructor
forms. Master and the current compiler produce an LLVM module verifier failure.

## Root cause

The imported C++ constructor thunk receives an LLVM `double` while its parameter
is `float`; thunk argument conversion does not truncate the floating value before
calling the constructor.

## Fix direction

Convert the floating argument to the selected floating parameter type with
`fptrunc` in all three construction forms, or report a source diagnostic before
module verification. Add value legs to the related existing C++ interop test.

- (C6 review 4) A pointer overload next to the float ctor does not change it: a double local into Fl(float)+Fl(int*) or Fd(float)+Fd(const char*) also fails module verification, identically on master.
