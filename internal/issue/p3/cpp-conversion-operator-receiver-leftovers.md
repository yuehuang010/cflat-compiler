# C++ conversion-operator receiver leftovers

Status: active. Landed: scalar ranking for `long`, `int` and bool context; explicit `operator bool` casts to long, i64,
char, double, float select `operator int()`; class-result copy-init (T105); receiver cv for `operator bool`,
`operator int()` and ternary arms; the `const` operator bool on a const receiver in every condition position (`if`,
`while`, `do`, `for`, `?:`, `&&`, `||`, `!`, `return c ? a : b`); const receiver of a derived class whose base declares
a const/non-const conversion pair.

Still open:

- T122 (pre-existing on master; class V with `operator int(); operator int() const;`): a template conversion
  `template<class U> operator U() const` next to a non-template `operator int()` on a const receiver. Clang picks the
  template (7). Since T122 CFlat refuses it (the raw "Argument mismatch detail" text was replaced by CONVOP, 2026-11-05: now
  "cannot convert value of type 'N.TT' to 'int'"; master before T122 silently picked the non-const non-template, 1).
  Closes when template conversions are instantiated.
- `template<class U> operator U()` is refused for every destination and position (int, long, double, class W; declaration
  init, assignment, call argument, explicit cast, bool context). Measured on a non-const `T` with only the template:
  clang uses the template (42) for each; CFlat reports "cannot convert value of type 'T' to 'int'" (or "no 'operator
  long' defined" for the cast form). Needs a clang wrapper for the conversion-operator template at the destination type
  (new clang requests; count them when landed), registration the scalar/class lookup sees, and the template-vs-
  non-template tie-break.

Legs: Test/test_cpp_interop.cb CONVOP block; header Test/library/cpp_t105_conversion_operators.hpp.
