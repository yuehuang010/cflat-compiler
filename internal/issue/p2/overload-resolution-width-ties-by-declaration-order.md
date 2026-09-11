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

The character types (`c8`/`c16`/`c32`/`wchar`, landed 2026-09-10) widen it: a `u16`
argument reaches `f(c16)`, a `c16` argument reaches `g(wchar)` on Windows, and declaring `f(u16)`
first flips the `c16` pick. Definition-time identity is right (`f(c16)` + `f(wchar)` are two
overloads); only the call-site pick ignores it.

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
   The full identity table, including `wchar` vs `c16`, is in the 2026-09-10 primitive-identity
   ruling at the bottom of `internal/fix-issue-lessons.md`.
2. Then value-preserving promotion (narrower to wider, same signedness; unsigned into STRICTLY
   wider signed).
3. Then conversion (same-width different-identity, narrowing, sign change).
4. A tie within the best tier is an ambiguity `LogError` naming the candidates - never first-wins,
   never last-wins.

Maintainer rulings (2026-09-10):

- **An unsuffixed integer literal is `int`**, as in C++: `f(5)` ranks exactly like an `int`
  argument, so it picks `f(int)` when one is declared.
- **Same-width different-identity binds as a conversion** (step 3), as in C++: a `long` argument
  with only `f(int)` declared (LLP64) still binds, ranked below any identity or promotion match.
- **A tie within the best tier is an ambiguity error** (step 4), never first-wins or last-wins.

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
therefore depends on declaration order in `cpp_interop_basic.h`. Since 2026-09-10 a C++ import
maps `long` and `long long` by identity, so the LP64 collapse may be gone; confirm leg 769 with the
fixture's declaration order reversed before counting that half fixed. Not part of this fix.

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

## Status

2026-09-11: landed on master, all four suites green (`test.bat`, `test_lsp.bat`,
`test_example.bat` Release). Verified independently: the new legs fail on the pre-fix binary
(`oi_c16_exact_a`, and the tie error never fires), and `--platform linux --check` over
`Test/test_*.cb` has no regression against it. Kept open for the gaps under "Left open".

What landed (`cflat/LLVMBackend_Overloads.cpp`):

- `RankIntegerConversion`: 0 identity-exact (`CanonicalPrimitiveTypeName`), then promotion
  (same signedness narrower to wider, or unsigned into a STRICTLY wider signed), then conversion.
  Only identity-exact is a perfect integer match. `u32` into an equal-width signed is a conversion.
- Promotion sub-order: C++ integral promotion into `int` first, then the narrowest destination.
  Equal-width promotions stay a tie (`u8` into `u16`/`i16`; `int` into `long`/`i64` on LP64).
- Candidate tier is its WORST integer argument; the lowest tier wins, then per-argument
  dominance, then the existing omitted/move tie-breaks. So `Test(name, u32v, 5)` picks
  `Test(i64, i64)` (all promotions) over `Test(int, int)` (one conversion).
- A remaining tie that only integer identity could decide is `ambiguous call to '<f>': no
  candidate ranks better than the others: <candidates>`. Ties at any other kind of position
  keep the legacy pick (perfect tier first, promotion tier last) - out of scope here.
- Identical parameter lists: the later registration shadows the earlier (a `program`'s own
  `void WaitForExit(int)` over the synthesized `bool WaitForExit(int)`).
- An unsuffixed literal ranks as `int` (else `long`/`i64`), via `NamedVariable::LiteralIdentity`
  stamped at the direct-call argument site. A signed argument's identity is read from
  `InferSourceTypeName`, since the call site drops its TypeName.
- View pair with known, different element identity (`int[4]` into `double[]`) scores implicit,
  not perfect, when there are several candidates.

Tests: `oi_*` legs in `Test/test_basic.cb` `testOverloadResolution`, each set declared in both
orders; `Test/errors/err_overload_ambiguous_tie.cb` (3 scoped legs, all 3 fire when run alone).
Cross target: `--platform linux` calls `_f$int$.1$int` / `_f$int$.1$long` / `_f$int$.1$i64`
for `int` / `long` / `i64` in both orders.

Moved in triage (first-wins flip moved 3 tests; no expectation changed, no `core/*.cb` edit):

- `test_program` "reserved name WaitForExit(int) different return": relied on the literal
  being a promotion plus last-wins; now decided by the identical-parameter shadowing rule.
- `test_math` `rng.shuffle(shuffled, 4)`: the literal became exact, which put both views in the
  perfect tier where first-wins took `double[]`; fixed by the view-element demotion above.
- `test_cpp_interop`: failed only under the flip (the file was also being edited concurrently);
  passes under the final rule.
- Intermediate dominance-only rule tied `Test(name, u32v, literal)` in `test_basic`, `test_c`
  and `test_generics`; resolved by the candidate-tier rule, no call site changed.

Left open:

- On LP64 the repro's `f(g)` (`u32` into `int`/`long`/`i64`) is now the ambiguity error (C++
  agrees); the `u32` accept leg is gated `if const (__WINDOWS__)`.
- Interface-method slot resolution (`ResolveInterfaceMethodSlot`) never reports the ambiguity
  (keeps the legacy pick), and its argument loop stamps no identity, so signed arguments and
  literals there rank by width as before. Same for `new T(...)` and operator-overload arguments.
- Char literals (`'a'`) and hex literals in u32 range past `INT_MAX` (they lower as an i32 bit
  pattern) have no identity and keep width ranking.
- The C++ constructor binder is unchanged (see "Related, out of scope here").
