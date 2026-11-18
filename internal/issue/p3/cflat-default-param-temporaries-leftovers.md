# CFlat default-parameter temporaries: remaining caller lifetime

Native defaults remain intentionally unchanged: a native `N` default temp can leak, explicit
`fRd(mkN(1), N(5))` can destroy `n5` twice, and native/unique temps built inside a C++ class
default can leak. They are outside this class-temporary timing item.

Related: omitted defaults on C++ functions use CDEFARG caller-slot initialization. CFlat class
defaults that resolve an earlier formal parameter stay in the forwarding wrapper because they
cannot be evaluated independently at the call site, and retain the wrapper's early-destroy behavior.

Round 3 residue: C++ class-typed, pointer/nullptr, string-literal, and nested-brace elements in
C++ class parameter defaults are refused with a clean diagnostic. Three shared-helper attempts
preserved class storage but still trapped or produced an extra constructor, and did not match
clang for nullptr/string arguments. Scalar non-null non-string brace elements remain supported;
the refused shapes match master's prior clean-refusal behavior. Earlier-parameter brace defaults
remain accepted because scalar `int f(int a, int b = a)` is also accepted by CFlat.
