# C++ primitive typing: C spellings as aliases, character types, one identity table

Maintainer ruling 2026-09-10 (revised the same day). CFlat ACCEPTS C's multi-word primitive
spellings and adds the C++ character types. This reverses the earlier "one primitive, one word"
ruling, which filed this issue as `reject-multi-word-primitive-spellings.md` with the opposite fix
direction. Ruling record: bottom of `internal/fix-issue-lessons.md`.

## The ruling

1. Every C multi-word spelling is an ALIAS. It canonicalizes to one CFlat word at parse time and is
   the same type as that word, not a look-alike: `long long` IS `i64`, `unsigned int` IS `u32`.
   Aliases add no type identity.
2. New primitives `c8`, `c16`, `c32`, `wchar`, with the C++ names `char8_t`, `char16_t`,
   `char32_t`, `wchar_t` as aliases for users who prefer the longer form.
3. CFlat primitive identity matches C++ one-to-one: two C++ types that mangle differently are two
   CFlat types, and each C++ primitive has exactly one CFlat identity.

## Identity table

| CFlat | Accepted aliases | C++ type | MSVC | Itanium |
|---|---|---|---|---|
| `char` | - | `char` | `D` | `c` |
| `i8` | `signed char` | `signed char` | `C` | `a` |
| `u8` | `unsigned char` | `unsigned char` | `E` | `h` |
| `short` = `i16` | `short int`, `signed short`, `signed short int` | `short` | `F` | `s` |
| `u16` | `unsigned short`, `unsigned short int` | `unsigned short` | `G` | `t` |
| `int` = `i32` | `signed`, `signed int` | `int` | `H` | `i` |
| `uint` = `u32` | `unsigned`, `unsigned int` | `unsigned int` | `I` | `j` |
| `long` | `long int`, `signed long`, `signed long int` | `long` | `J` | `l` |
| `ulong` | `unsigned long`, `unsigned long int` | `unsigned long` | `K` | `m` |
| `i64` | `long long`, `long long int`, `signed long long`, `signed long long int` | `long long` | `_J` | `x` |
| `u64` | `unsigned long long`, `unsigned long long int` | `unsigned long long` | `_K` | `y` |
| `i128` | `__int128` | `__int128` | - | `n` |
| `u128` | `unsigned __int128` | `unsigned __int128` | - | `o` |
| `longdouble` | `long double` | `long double` | `O` | `e` |
| `c8` | `char8_t` | `char8_t` | `_Q` | `Du` |
| `c16` | `char16_t` | `char16_t` | `_S` | `Ds` |
| `c32` | `char32_t` | `char32_t` | `_U` | `Di` |
| `wchar` | `wchar_t` | `wchar_t` | `_W` | `w` |

Widths: `c8`/`c16`/`c32` are 8/16/32-bit unsigned on every target. `wchar` is target-native like
`long`: 16-bit unsigned on Windows, 32-bit signed on x86-64 Linux and on macOS, 32-bit unsigned on
aarch64 Linux. On Windows `wchar` and `c16` have the same width and encoding and are STILL two
types (`_W` vs `_S`, `std::wstring` vs `std::u16string`) - the same shape as `int` vs `long`.

## Current state

Two partial alias mechanisms that disagree by position (measured before the first ruling):

```cflat
std.vector<long long> v = default;   // accepted, canonicalizes to i64 (template-arg table)
long long b = 5;                     // accepted, canonicalizes to i64 (LongSpellingTypeName)
unsigned int a = 5;                  // cannot find the type 'unsigned'
list<long double> x = default;       // cannot find the type 'longdouble'   <- pseudo-token leak
```

- `LongSpellingTypeName` (`cflat/MainListener.h:170-177`) counts `long` specifiers, called from
  both `ParseDeclarationSpecifiers` copies (`cflat/ForwardRefScanner.cpp:62-67,287`,
  `cflat/MainListener_Declarations.cpp:560-562,936`). Handles `long long` only.
- `multiWordTypeSuffix` (`cflat/CFlat.g4`, added by `6f1981c`) + `CanonicalTemplateTypeArgument`
  (`cflat/MainListener.h:178-205`) run in a template-argument position alone.
- Identity fold `PrimitiveCanonicalNames` (`cflat/TypeMangling.cpp:112`) has `i16`->`short` and
  `i32`->`int` only; `uint`/`u32` is missing.
- Outbound `CxxSpellingForCflatType` (`cflat/LLVMBackend_CInterop.cpp:3191-3198`): `long`/`ulong`
  spell as `long long` (filed separately, see Sequencing); no character-type rows.
- Inbound scalar map (`cflat/LLVMBackend_CInterop.cpp:1158-1191`): `char16_t`->`u16`,
  `char32_t`->`u32`, `wchar_t`->`u16`/`i32`, `char8_t` absent. Identity is lost on the way in.

## Work

1. **One canonicalizer, every type position.** Declaration, parameter, field, return, cast,
   `sizeof`, template argument. Replace both partial mechanisms with it, called from BOTH
   `ParseDeclarationSpecifiers` copies. `signed`/`unsigned` are soft keywords: text-match them,
   do not add lexer tokens (CLAUDE.md "New soft keyword"). Downstream code only ever sees the
   CFlat word.
2. **Reject invalid combinations** with `LogError` quoting the spelling the user WROTE:
   `long long long`, `short long`, `unsigned float`, `signed bool`, a C modifier on a CFlat width
   word (`unsigned i32`). The `cannot find the type 'longdouble'` shape must not survive.
3. **New primitives** `c8`/`c16`/`c32`/`wchar`: integer types (arithmetic, promote to `int` like
   C++, promote to `int` through varargs). `wchar` needs a target seam beside
   `SetTargetLongWidth` (`cflat/LLVMBackend_OwnershipTemps.cpp:57`) carrying width AND
   signedness. Any new field an analysis reads goes into the `--init` cache round-trip in the same
   change.
4. **Identity fold**: add `uint`==`u32` to `PrimitiveCanonicalNames` (first check whether
   `ResolveManglingAlias` already folds it). `f(long long)` + `f(i64)` must be a redefinition;
   `f(long)` + `f(i64)` must stay two overloads.
5. **Outbound**: `c8`->`char8_t`, `c16`->`char16_t`, `c32`->`char32_t`, `wchar`->`wchar_t`.
6. **Inbound**: `char8_t`/`char16_t`/`char32_t`/`wchar_t` -> `c8`/`c16`/`c32`/`wchar`, subject to
   the boundary ruling below.
7. **Docs**: `doc/LANGUAGE.md` "Primitives" gains the character types when they land (the alias
   table is already there).

## Rulings needed before building

- **C boundary identity.** Win32 is full of `DWORD*` (`unsigned long*`) and `LPWSTR`
  (`wchar_t*`). Today they bind as `u32*` and `u16*`, and `core/wstring.cb`, `winrt.cb` and every
  Win32 demo rely on that. Mapping C headers inbound by identity turns those into `ulong*` and
  `wchar*` and breaks every such call site. Recommended: split by boundary. C headers stay
  width-mapped (C symbols are unmangled, so width is all the ABI sees); C++ binding maps by
  identity. The alternative is identity everywhere plus implicit same-width pointer conversion at
  unmangled C calls. Measure the blast radius of each before choosing.
- **`wchar` spelling.** Chosen by analogy with `c16` + `char16_t`; confirm.
- **Character literal prefixes** (`u8'a'`, `u'a'`, `U'a'`, `L'a'`) are out of scope - file as a
  `p4/` item if wanted.

## Sequencing

1. [`c-long-has-no-cflat-cxx-spelling.md`](c-long-has-no-cflat-cxx-spelling.md) first: the
   `long`/`ulong` outbound rows are the one silent wrong answer in the table today.
2. This issue.
3. [`../p2/overload-resolution-width-ties-by-declaration-order.md`](../p2/overload-resolution-width-ties-by-declaration-order.md)
   consumes this identity table for its identity-exact tier.

## Acceptance

- Every alias in the table resolves to its CFlat word in statement, parameter, field, cast,
  `sizeof` and template-argument position.
- `f(long long)` + `f(i64)` and `f(unsigned)` + `f(u32)` are redefinition errors;
  `f(long)` + `f(i64)` and `f(wchar)` + `f(c16)` are distinct overloads on every target.
- win64 IR: `std.vector<X>` instantiates `J` for `long`, `K` for `unsigned long`, `_J` for
  `long long`, `_S` for `c16` and `char16_t`, `_U` for `c32`, `_Q` for `c8`, `_W` for `wchar`.
  Itanium letters are verified on a Linux host; C++ interop cross-targeting from a Windows host
  does not work today ("does not name a C++ class type").
- `sizeof`: `c8` 1, `c16` 2, `c32` 4, `wchar` 2 on win64 and 4 on linux (constant-folded in IR,
  checkable cross-target).
- A C++ member returning `char16_t` reads back as `c16` and re-spells to the same specialization.
- Invalid combinations are errors naming the written spelling - new `Test/errors/err_*.cb`.
- Accept legs extend `Test/test_cpp_interop.cb` (multi-word coverage, M21 neighbourhood) and
  `Test/test_basic.cb`; no new test file. Legs 769 and 793 stay green unchanged.
- `test.bat Release` and `test_lsp.bat` green (`MainListener.h` changes).
