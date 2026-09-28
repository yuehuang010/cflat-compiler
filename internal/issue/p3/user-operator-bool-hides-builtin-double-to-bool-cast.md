# A free `bool operator bool(Box)` hides the built-in `(bool)<double>` cast

Pre-existing (found 2026-09-27 by fix/bool-fp): in Test/test_operators.cb the free
`bool operator bool(TruthinessBox)` makes `(bool)d` with `double d` fail: "no overload of 'operator bool'".
A user conversion operator for one struct type must not shadow the builtin scalar cast for other types.
Fix direction: in the cast path, try the user `operator T` only when the operand is a struct/class type
that has one; scalars always take the builtin conversion.
