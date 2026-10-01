# `5L` is i64 to cflat but C++ `long` to clang on Windows: valid `std.min(x, 5L)` fails to compile

Found 2026-09-28 on the first Windows run of the C++ interop suite (Test/test_cpp_interop_template.cb
line ~3382, return 2629, `long mmLong = 3L; if (std.min(mmLong, 5L) != 3L)`). Compiles and passes on macOS.
On Windows (LLP64, `long` = 32-bit) the `L` suffix is incoherent:
- cflat types `5L` as i64 (`sizeof(5L)` = 8, `sizeof(long)` = 4; `--run` probe exits 84 for
  `sizeof(5L) * 10 + sizeof(long)`).
- the C++ request spells the same literal with C++ `long` identity.
Consequences, all with a plain `-o` compile (`--check` does NOT reproduce - it skips this step):
- `long c = 3L; std.min(c, 5L)` -> "no overload of 'std.min' matches" (candidates `std.min(long*, long*)`;
  arg [1] is i64, which cannot bind `const long&`). Inside the full test it surfaces as
  "parameter 'p0' of 'std.min' takes ownership of the value; pass 'move <arg>'".
- `long long a = 3L; std.min(a, 5L)` -> clang "no matching function for call to 'min'" (literal is `long`).
- Works: `5LL` with `long long`, or two `long long` variables.
Repros: scratch/lsuffix/min_seq.cb, min_lit_L.cb.
No working spelling of the literal matches `long` on Windows, so the test cannot be written portably;
it is intentionally left failing on Windows (tests represent real usage) until this is fixed.
Fix direction (needs maintainer ruling on the surface; doc/LANGUAGE.md "Default literal width" names
`123L` but not its width): make `L` mean the target's C `long` (32-bit on Windows, 64-bit on LP64) and
`LL` mean i64, matching C and the C++ identity the request already uses. Check every site that types a
suffixed literal (MainListener_Utilities.cpp ParseNumberConstant/ParseLiteralTypeAndValue,
MainListener.h ParseScannerIntegerLiteral, MainListener_Aggregates.cpp suffix list) and the literal ->
C++ spelling path so both agree.

RULING 2026-09-30 (maintainer): `L` is platform-specific - the target C `long` (32-bit on LLP64/Windows, 64-bit on LP64); `LL` is always i64. Literal typing and the C++ spelling path must agree.
