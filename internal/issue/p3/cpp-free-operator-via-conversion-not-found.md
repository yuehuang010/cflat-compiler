# Free C++ operator reached only through a conversion function or converting constructor is not found

Summary: a free C++ operator whose operand needs a user-defined conversion (a conversion function on
the argument class, or a converting constructor of the parameter class) gives "no operator" in cflat;
clang++ -std=c++20 selects it (T116 review 2 probe values 50 / 51).

Found: T116 review 2 (2026-10-11), pre-existing on master; T116's free-operator candidate filter
(LLVMBackend_Overloads.cpp ~4718, CanImplicitlyConstructCxxClass) does not change these cells.

Fix direction: let free-operator candidate viability consider one user-defined conversion per
operand, as call arguments already do (TryBindCxxImplicitArgumentConversions); compare against clang
for both the conversion-function and converting-ctor spellings.
