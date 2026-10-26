# Borrowed-value diagnostic exposes mangled generic names

Summary: Unique declaration-initializer borrowed-value diagnostics are not invertible and omit context for methods.
Repro: Trigger the unique initializer borrowed-value diagnostic in a generic function and in a method.
Master vs expected: master prints a name such as `pick$bool` and bare `from a borrowed value`; expected demangled generic names and contextual method wording.
Found: T443 r4 haiku review, 2026-10-24 box, PRE-EXISTING
