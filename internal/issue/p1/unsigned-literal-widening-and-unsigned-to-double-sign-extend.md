# Two silent unsignedness losses in codegen: a `u`-suffixed literal sign-extends, and
# unsigned -> double uses a SIGNED conversion

Filed 2026-08-21 from an external report (v0.11.0 issue 15 - an xorshift PRNG in the reporter's
`src/pead.cb`: `(s >> 11) & 4294967295u` never masked anything, so every "uniform in [0,1)" draw
came out in the hundreds of thousands). Reproduced and narrowed on `39d4b38`, Release.

Severity: **silent wrong arithmetic**. No diagnostic, no crash. A 32-bit mask silently becomes
all-ones, so `&` becomes a no-op.

## Defect 1 - a `u`-suffixed literal that fits in 32 bits sign-extends when used at u64 width

```cflat
u64 mask = 4294967295u;      // 0xFFFFFFFF  -> prints 18446744073709551615
u64 t    = 4294967296u;      // 2^32
(t & 4294967295u)            // -> 4294967296   (expected 0)
(t & 0xFFFFFFFFu)            // -> 4294967296   (expected 0)
(t & mask)                   // -> 4294967296   (expected 0)
```

Narrowing (measured):

| Expression | Result |
|------------|--------|
| `u32 v = 4294967295u; u64 a = v;` | **4294967295** - correct; widening a u32 VARIABLE is fine |
| `u64 b = 4294967295u;` | **18446744073709551615** - WRONG, sign-extended |
| `u64 c = 4294967295;` (unsuffixed) | **4294967295** - correct |
| `u32 s = 65535u; u64 d = s;` | 65535 - correct (no high bit set, so the bug is invisible) |

So this is not the general widening path - that one is correct. It is the constant itself: a
`u`-suffixed literal is materialised at u32 width and then widened with `sext` instead of `zext`
(or the constant is stored as a signed 64-bit value before the suffix is applied). The unsuffixed
literal takes a different path and is correct, which is why the suite has never caught it.

Note the interaction with [[integer-literal-typed-as-smallest-fitting-type]]: literal typing is
value-driven, so the exact width a literal is materialised at is already subtle. Fix that width
question first if the two turn out to share a code path.

## Defect 2 - unsigned -> double is a SIGNED conversion (`sitofp`, should be `uitofp`)

```cflat
u64 big = 18446744073709551615u;  // prints correctly via toString()
printf("%f", (double)big);        // -> -1.000000   (expected 1.8446744e+19)

u32 x = 4294967295u;
printf("%f", (double)x);          // -> -1.000000   (expected 4294967295.0)
```

Independent of defect 1: the u32 VARIABLE case above is materialised correctly (it prints
4294967295 as an integer) and still converts to `-1.0`. The cast is emitting `sitofp` regardless
of the source's signedness. This is what turned the reporter's PRNG output into garbage even after
they worked around the mask.

`i64` with unsuffixed literals is correct throughout (`4294967296 & 4294967295 == 0`), which is the
workaround the reporter settled on.

## Fix direction

Both are the same class as [[operator-overload-result-signedness-dropped]] (p2): the signedness
carried on `TypeAndValue` is available at the site and is not consulted. That issue records the
established fix pattern - branch on `IsUnsignedInteger() != -1` rather than assuming signed.

- Defect 1: at literal materialisation / integer widening, select `zext` vs `sext` from the
  literal's own unsignedness (the `u` suffix), not from the destination or from a default.
- Defect 2: at the int -> float cast, select `uitofp` vs `sitofp` from the source's signedness.

## Regression test

Extend an existing test (`Test/test_operators.cb` has the shift/signedness cases from the
2026-08-15 work) with: `u64 t = 4294967296u; assert((t & 4294967295u) == 0);`, the `u32 v` ->
`u64` leg, and `(double)18446744073709551615u > 1.8e19`. All three are single-line asserts.

## Adjacent

- [[operator-overload-result-signedness-dropped]] - same family (signedness dropped in codegen),
  operator-overload branches; carries the fix pattern and the prior probe methodology.
- [[integer-literal-typed-as-smallest-fitting-type]] - literal width/typing rules.
