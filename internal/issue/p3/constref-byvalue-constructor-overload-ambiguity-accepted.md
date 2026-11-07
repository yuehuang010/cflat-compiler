# Const-reference and by-value overload tie through a conversion function still accepted

Narrowed by CVAMB round 2. Fixed there: ties through the same converting constructor
(`ps("abc")` with `ps(const std::string&)` + `ps(std::string)`, `fi(5)` with `fi(const WV&)` + `fi(WV)`).

Still open (clang++ -std=c++20 says ambiguous, CFlat accepts):
- `fi(u)` with `fi(const WV&)` + `fi(WV)` and `struct U { operator WV() const; }`: CFlat picks 1, clang
  "call to 'fi' is ambiguous" (repro scratch/rev_cvamb_repro/conv_op.cb). The pair never reaches
  sameClassIdentityMix (a conversion-function argument is ordered before that check; not yet traced).
  Fix direction: find the comparison that orders the pair, and tie it when both candidates use the same
  conversion function with the same second standard conversion.
