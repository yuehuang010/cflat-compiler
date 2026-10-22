# `for (move b.f; ...)` with an unparenthesized move expression as for-init crashes (139)

Severity p2 (compiler crash on valid-looking input). Found by the T352 implementer (2026-10-20); pre-existing on master 08d630878.

## Repro
```cflat
struct B { string f = default; };
extern int main() { B b = default; b.f = "x"; for (move b.f; false; ) { } return 0; }
```
master: exit 139 "cflat internal compiler error". `for ((move b.f); false; )` compiles; `for (i + 1; false; )` compiles.
T352 named ParseForDeclaration as the crash site (not yet root-caused): the for-init probably parses `move b.f` as a declaration
(`move` is a soft keyword read by ParseDeclarationSpecifiers, so `move b` looks like type `b` + declarator `.f`?).

## Fix direction
Root-cause with lldb first. Either the for-init declaration path must reject a non-declaration cleanly (LogError) or the soft keyword
`move` must not start a for-init declaration; never an internal error. Leg: Test/test_move.cb discard-sink for-init legs (T352 added
the parenthesized form in Test/test_collection_leaks.cb ~1973).
