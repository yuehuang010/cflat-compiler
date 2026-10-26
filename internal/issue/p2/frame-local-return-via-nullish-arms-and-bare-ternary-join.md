# Bare statement conditional joins skip pointer-join checks

Remaining T423 follow-up after the nullish frame-local return fix.

- A value-context join of unrelated record pointers is rejected, but the same join is accepted when its result is discarded as a bare statement.
- Repro: `struct T423BareA { int a = 1; }; struct T423BareB { int b = 2; }; extern int main() { T423BareA a = default; T423BareB b = default; bool c = true; c ? &a : &b; return 0; }`
- Measured on master: the bare statement links; assigning `c ? &a : &b` to `T423ValueA*` reports `cannot join 'T423ValueA*' and 'T423ValueB*' in a '?:'`.
- `ParseStatement` routes a discarded assignment expression through `ParseAssignmentExpressionNamed(..., ResultUse::Discard)`; the value join diagnostics are in `ParseTernaryBranches`. The discarded-expression path needs its own coverage and fix.
