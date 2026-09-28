# Free compound-assignment operator template with a class operand fails to compile

Found 2026-09-27 (fix/op-fn-temp). `template<class F> Op& operator+=(Op&, F)` used as
`o += rv.S(7)` never reaches the C++ operator-template route; compile fails with "C++ 'operator+' cannot
bind its right operand because the CFlat expression is an rvalue and the parameter is a non-const
reference." (note: it also names operator+ for a +=). Pre-existing on master e6c60220. clang++ -std=c++20 accepts and calls the template.

Fix direction: route compound assignment on a C++ class LHS through the same free operator-template
lookup as the binary form (`callOperatorTemplate` in TryBinaryOperatorOverload,
MainListener_Expressions.cpp) before falling back to the scalar store path. Fixture beside FopSelfRef in
Test/library/cpp_interop_freeoptpl.h, legs near 2142-2145 in Test/test_cpp_interop_template.cb.
