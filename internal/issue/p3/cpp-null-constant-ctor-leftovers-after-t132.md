# Literal-zero C++ constructor leftovers after T132

PRE-EXISTING / follow-ups (T132 reviews 1-3, 2026-10-12).

1. By-value `r.takeSC(0)` with SC(char) + SC(int*): refused (correct) but with "cannot create a hidden
   class argument for 'r.takeSC'"; clang says "conversion from 'int' to 'r::SC' is ambiguous". Report the
   null tie in MaterializeImplicitCxxClassArgument's zero-literal path (LLVMBackend_CInterop.cpp
   ~28714) with the same ambiguity error as copy-init (S7 by-value already does).
2. `r.SCL(5)` with SC(char) + SC(long) direct: CFlat 33, clang ambiguous; `SCL s = 5;` refused with a
   different text.
3. `r.SCo(300)`, `takeSCo(0 | 300)` (char-only ctor) refused by CFlat's no-implicit-narrowing rule;
   clang accepts (12 / 11 / 12). By design unless ruled otherwise.
4. `r.EX e = 0;` / `= 0L` (explicit EX(long) + EX(void*)) refused "cannot initialize"; clang 22.
5. A CFlat struct field default of a C++ class type with `= 0` is not applied (`r.K k = 0;`,
   `r.SInt s = 0;` fields read 0; clang 1 / 11); field `r.S7 s = 0;` accepted where clang is ambiguous.
6. `takeK(z)` with an int variable refused with EMPTY error text (clang: no viable conversion).
