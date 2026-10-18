# C++ compound assignment on an xvalue or free-operator receiver mishandled

PRE-EXISTING (T127 reviews 2 and 3; same on master).

The reversed `<=>` relation item from this issue is fixed on master by
`753d9b22f5510ebe3f31a1fd1962440ee9edd83e` (T291). On 2026-10-17, the supplied
master executable matched clang for `0 < w`, `0 > w`, `0 <= w`, `0 >= w`, and
`w < 0` with member/free operators, strong/partial/weak ordering, and a template
class. Do not reopen that item without a new failing shape.

## Remaining item: compound assignment on non-lvalue receivers

Clang accepts `rref() -= 1` when `rref` returns `T&&` and selects the `&&` member.
The supplied master executable rejects this form with
`Left side of assignment is not an addressable lvalue`.

For `(move l) -= 1`, clang selects the member `&&` overload. Master currently
compiles the expression but selects the `&` overload. For a prvalue `mkF()` with
free `operator-=(F&, int)` and `operator-=(F&&, int)`, clang selects `F&&` while
master rejects the expression with the same addressability diagnostic.

These are distinct handling paths: a `T&&` call is refused, an explicit move
reaches the member overload but loses its xvalue category, and a free operator
is rejected by the rvalue assignment path. Keep this item open for a coverage
matrix and fix that preserves value category and temporary lifetime across all
three forms. The existing member-on-prvalue form (`mk() -= 1`) passes via the
T127 r3 compound-assignment path and remains a control case.
