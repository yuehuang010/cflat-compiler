# C++ scalar `T&&` return is read as the referent's ADDRESS (silent wrong value)

A C++ function or member whose return type is an rvalue reference to a scalar (`long&&`,
`size_t&&`) yields the referent's address converted to an integer, not the value. No
diagnostic. Found by the libtorch ladder (2026-09-26): t13 `(long)ds.size().value()` prints
`n=6155921288` (a stack address) instead of 4.

## Repro

`scratch/refbug/rr.h` / `rr.cb` (all three reads are wrong on master cfdbeca4):

```cpp
#pragma once
namespace rr {
struct S { long v = 7; S(int) {} long&& take() { return static_cast<long&&>(v); }
           long&& take2() && { return static_cast<long&&>(v); } };
inline long g_v = 9;
inline long&& free_take() { return static_cast<long&&>(g_v); }
inline S make() { return S(0); }
}
```

```cflat
import cpp "rr.h";
extern int printf(const char* fmt, ...);
extern int main()
{
    rr.S s = rr.S(0);
    long a = s.take();          // expect 7
    long b = rr.free_take();    // expect 9
    long c = rr.make().take2(); // expect 7
    printf("a=%ld b=%ld c=%ld\n", a, b, c);
    return (a == 7 && b == 9 && c == 7) ? 0 : 1;
}
```

Output: `a=6126299728 b=4340613416 c=6126299736`, exit 1.

## History

Latent since at least 129c64a8 (2026-09-13): that compiler gives the same wrong values on rr.cb.
It became visible on libtorch at 44e18016 ("Respect C++ member ref-qualifiers in overload
resolution"; `git bisect` on scratch/refbug/rb.cb). Before that commit, `optional::value()` on a
temporary receiver bound the `const T& value() const&` overload (lvalue-ref return, handled as
`alias T`). Since then it correctly binds `T&& value() &&`, which hits this bug. The ref-qualifier
change is right; the return mapping is wrong.

## Root cause

`LLVMBackend_CInterop.cpp` (around line 13065), `asAliasIfRef` in the member/function signature
mapper: `CxxReferenceKind::Rvalue` returns early, so a `long&&` return keeps the scalar
mapper's `long*` shape instead of becoming `alias long` like an lvalue-reference return does.
The call site then initializes `long a` from a `long*` with no diagnostic (the separate
pointer-to-integer gate gap, see below), which produces the address.

The existing fixture only covers a CLASS rvalue-reference return
(`M81RefqMethods&& with(int) &&` in `Test/library/cpp_interop_tpl.h`), never a scalar one.

## Fix direction

Treat an rvalue-reference RETURN like an lvalue-reference return for reading: the call is an
xvalue, so `long a = f();` must load the referent. Mapping it to `alias T` (or loading at the
call site) and keeping the Itanium pointer ABI is the natural fit. Check that class `T&&` returns
(the `with()` fixture, move-construct from an xvalue) keep their current shape. Regression: add
the scalar `&&` shapes above to `cpp_interop_tpl.h` + `Test/test_cpp_interop.cb`, including a
`std::optional<size_t>` temporary `.value()`.

## Related ruling (2026-09-26)

Plain CFlat silently accepted `long* p = &v; long x = p;`, which is what let this bug through
without a diagnostic. Maintainer ruling: CFlat pointers are not numbers, an explicit cast is
required - an oversight, filed separately as `implicit-pointer-to-integer-accepted-at-stores.md`.

## Notes for the fixing agent

- The repro above is complete and self-contained (in-repo headers only, no libtorch); compile with
  `x64/Release/cflat rr.cb -o rr && ./rr`. Compiles in about 1 s.
- Also fix-verify on the libtorch ladder rung that found it (local-only, `scratch/` is gitignored -
  skip if absent): `bash scratch/ladder/torch/run_all.sh t13` -> must print `n=4 ex0=2`, run_rc=0.
- Sequencing with `implicit-pointer-to-integer-accepted-at-stores.md`: if that gate lands first,
  this repro becomes a COMPILE ERROR ("pointer is not a number") instead of a wrong value. Still a
  bug - the `&&` return must read the referent. Do not "fix" it by adding a cast in the test.
- Before editing, confirm the mapping with `--out-lli`: `@..._take` returns `ptr`, and the caller
  does a `ptrtoint` into `%a`. After the fix, expect a `load i64` from the returned pointer.
- Class-typed `T&&` returns (fixture `M81RefqMethods&& with(int) &&` in
  `Test/library/cpp_interop_tpl.h`, used in `Test/test_cpp_interop.cb`) already work; keep them green.
- Where to extend: `Test/library/cpp_interop_tpl.h` (fixture) + `Test/test_cpp_interop.cb` legs inside
  `main` before `return 0;`. No new test files. Header cache: if the change alters what extraction
  records for a signature, bump the header cache version (`LLVMBackend.h`, see the torchreg round-2
  precedent), otherwise a warm cache keeps the old `long*` mapping and the test goes vacuous -
  verify on a fresh `CFLAT_CACHE_DIR` AND warm.
