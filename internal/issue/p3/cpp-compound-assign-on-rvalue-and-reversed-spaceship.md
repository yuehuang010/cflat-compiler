# C++ operators: compound assignment on an rvalue receiver refused; reversed <=> rewrite keeps the relation

PRE-EXISTING (T127 reviews 2 and 3; same on master).

## 1. Compound assignment / `=` on an rvalue that is not a materialized prvalue
`rref() -= 1` (call returning `T&&`), `(move l) -= 1`, and `mkF() -= 1` where the operator is a FREE
`operator-=(F&, int)` / `operator-=(F&&, int)` are refused with "Left side of assignment is not an
addressable lvalue". clang++ -std=c++20 accepts them and picks the `&&` member / `F&&` free overload.
The member-on-prvalue form (`mk() -= 1`) works since T127 r3 via the prvalue compound block in
MainListener_Expressions.cpp (~2340, `lhsMaterializedPrvalue`); the xvalue and free-operator paths
never reach it.

## 2. Reversed rewritten `<=>` does not flip the relation
`0 < w` where only `w <=> int` exists: C++ rewrites to `0 < (w <=> 0)` reversed, i.e.
`(w <=> 0) > 0`. CFlat evaluates it with the unflipped relation. Repro shape: struct W with
`std::strong_ordering operator<=>(int) const`, compare `0 < w` vs `w > 0` for w > 0 and w < 0.

Fix direction: (1) route xvalue / free-operator compound receivers through the same materialize-or-
address path as the prvalue member case; (2) in the reversedCandidate branch (~14508) swap the
relation when the rewritten candidate is <=>.
