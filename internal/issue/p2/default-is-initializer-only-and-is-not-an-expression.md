# `default` in a ternary arm still has no destination type

Filed 2026-08-21; the bulk of it was FIXED in the p2 bundle (`default` is now a
`primaryExpression` in `CFlat.g4`, typed from `declExpectedType`). What remains is the ternary
arm - and, generally, any position where the destination type has been withdrawn.

## What now works (measured, Release)

| Position | Result |
|----------|--------|
| `p = default;` (assignment RHS, struct) | OK - fields back to declared initializers |
| `p = default;` with an owning field | OK, leak-clean (destructs first, no memset) |
| `takes(default)` (call argument) | OK |
| `return default;` (in a `P`-returning function) | OK |
| `int i = default;` (primitive) | OK |
| `P p = default;` (declaration, the original form) | OK |

## Residual

```cflat
struct P { int a = 0; };
extern int main() { bool h = true; P q = h ? default : default; return 0; }
```

```
(2,45): 'default' needs a known target type here - it takes its value from the destination,
and this position supplies none. Write the type explicitly (e.g. 'T x = default;' and use 'x'),
or cast the destination.
```

This is now a clear, attributed diagnostic rather than the old parser mismatch
("expecting {'alignof', 'simd', ...}"), so the discoverability half of the original report is
addressed. The reporter's real shape - `AppHistory old = hasOld ? _appHistory.get(key) : default;`
- still needs the declaration-plus-`if` rewrite.

## Root cause of the residual

`DeclExpectedTypeGate` deliberately WITHDRAWS `declExpectedType` at the binary/ternary level unless
the level is a pure pass-through, so by the time `ParsePrimaryExpression` sees the `default` token
inside a ternary arm the destination type is empty and it takes the reject path.

## Fix direction (attempted and reverted - read before retrying)

Plumbing a `ternaryOuterExpectedType` through `ParseConditionalExpression` /
`ParseTernaryBranches` and re-publishing it for the arms was implemented and **measured not to
fire** - the arm still reported the unknown-destination error, so the gate is not the only place
the type is dropped. It was reverted rather than shipped as machinery no leg could exercise.
Anyone retrying this must first find where the arm's expected type is actually lost (instrument
`declExpectedType` at arm entry), and must keep a leg that FAILS before the change.

Note the ternary must agree on ONE type across both arms; `h ? default : default` has no type at
all from either side and should keep rejecting even after a fix.

## Regression coverage

`Test/test_core.cb::testDefaultAsExpression()` covers the accepted legs above by value.
