# Native overload resolution ranks by width, and breaks ties by declaration order

Found 2026-09-10 while checking how CFlat decides between `int` and `long`. Measured on
`x64/Release/cflat.exe` at `9e3846a`. A silent wrong answer: the call compiles, runs, and reaches a
different function than the one whose parameter type the argument exactly matches.

## Repro

```cflat
int f(int x)  { return 1; }
int f(long x) { return 2; }
int f(i64 x)  { return 3; }
extern int main()
{
    int a = 1; long b = 1; i64 c = 1; i16 e = 1; u32 g = 1;
    printf("%d %d %d %d %d %d\n", f(a), f(b), f(c), f(e), f(g), f(5));
    return 0;
}
```

Run with `--run` on win64 (`int` and `long` both 32-bit), then again with the three declarations
reversed:

| argument | order int, long, i64 | order i64, long, int |
|---|---|---|
| `int` | f(int) | **f(long)** |
| `long` | **f(int)** | f(long) |
| `i64` | f(i64) | f(i64) |
| `i16` | f(i64) | f(int) |
| `u32` | f(i64) | f(int) |
| literal `5` | f(i64) | f(int) |

Six of seven picks move when only the declaration order changes. An exact type match loses in both
orders (`long` -> `f(int)`, then `int` -> `f(long)`). No call is ever diagnosed as ambiguous.
LP64 shows the same shape between `long` and `i64` (both 64-bit).

## Root cause

`LLVMBackend::CreateOverloadedFunctionCall` (`cflat/LLVMBackend_Overloads.cpp`) scores integers by
WIDTH and signedness, never by type identity, and settles ties by position:

- `:317-324` - same width, same signedness scores `0` (perfect). On LLP64 `int` and `long` are both
  32 bits, so each is a perfect match for the other. The comment there (`long==i64`) is also stale
  for Windows, where `long` is 32-bit.
- `:513` - perfect tier compares with strict `>`: the FIRST declared perfect candidate wins.
- `:538` - promotion/implicit tier compares with `>=` ("keeps the pre-existing last-wins tie"): the
  LAST declared candidate wins.
- `:326-328` - an unsigned source into a signed parameter "at equal or greater width" is scored a
  safe implicit conversion. At EQUAL width it is not value-preserving (`u32` values from 2^31 wrap
  in `int`). This is how `u32` reached `f(int)` in the reversed order. (The ternary there,
  `(myBits == otherBits) ? 1 : 1`, is dead - both arms are 1.)

The identity layer disagrees with the resolver. `PrimitiveCanonicalNames`
(`cflat/TypeMangling.cpp:112`) folds only `i16`->`short` and `i32`->`int`, so declaring `f(int)` and
`f(i32)` is a redefinition error, but `f(long)` with `f(i64)` - equal width on LP64 - is ACCEPTED
as two distinct overloads (verified with `--platform linux --check`). CFlat lets the user declare a
distinction that its resolver then cannot see.

## Fix direction

Rank the way C++ does, in CFlat's own type terms:

1. Identity-exact first: parameter and argument equal after `CanonicalPrimitiveSpelling`
   (`int`==`i32`, `short`==`i16`; `long` distinct from both `int` and `i64` on every target).
   The full table, including `wchar` vs `c16`, is in `internal/issue/cppinterop/cxx-primitive-typing.md`.
2. Then value-preserving promotion (narrower to wider, same signedness; unsigned into STRICTLY
   wider signed).
3. Then conversion (same-width different-identity, narrowing, sign change).
4. A tie within the best tier is an ambiguity `LogError` naming the candidates - never first-wins,
   never last-wins.

Needs a maintainer ruling before building:

- **Unsuffixed integer literal.** `f(5)` today follows the last-wins tie. C++ types the literal
  `int`, so it picks `f(int)`. Pick the rule for CFlat (literal as `int`, or smallest-fitting with
  promotion) - it decides which call sites move.
- **Same-width different-identity** (`long` arg, only `f(int)` declared, LLP64): conversion (step 3,
  still binds) or refused? C++ binds it as a conversion.

## Blast radius - expect existing tests to move

`:538`'s comment says the last-wins tie was deliberately KEPT, so some existing call site depends on
it. Before editing, find it: run the suite with the tie flipped to first-wins and see what breaks.
Every moved leg must be triaged as either (a) was relying on the bug - fix the leg's EXPECTATION with
a note, or (b) exposes a genuine ambiguity the new rule must diagnose. Never dilute an assertion to
go green.

Core libraries overload heavily on integer widths; `core/*.cb` is compiled with every program, so a
new ambiguity error there surfaces in every test at once. Run `test.bat Release` after the first
edit, not the last.

## Related, out of scope here

The C++ constructor binder (`cflat/LLVMBackend_CInterop.cpp:9087`) has the same shape: it counts
type-name-exact parameters AFTER the inbound map, which on LP64 collapses `long` and `long long` to
`i64`, and keeps the first declared on an equal-shape tie. Test leg 769 (`Test/test_cpp_interop.cb`)
therefore depends on declaration order in `cpp_interop_basic.h`. That half lands with the identity
table in `internal/issue/cppinterop/cxx-primitive-typing.md` and is not part of this fix.

## Acceptance

- The repro picks `f(int)`, `f(long)`, `f(i64)` for `int`, `long`, `i64` in BOTH declaration orders,
  on win64 and on `--platform linux` (check the IR call targets for the cross target).
- `i16` resolves by promotion identically in both orders.
- A genuinely ambiguous call is an error, with an `err_` leg in `Test/errors/` whose substring names
  both candidates.
- `u32` into an equal-width signed parameter no longer scores as a safe conversion.
- Accept legs extend the overload coverage in `Test/test_basic.cb`, each asserted in both
  declaration orders (the order-independence IS the assertion; a single-order leg proves nothing).
- `test.bat Release` green, with every moved pre-existing leg triaged per "Blast radius" above.
