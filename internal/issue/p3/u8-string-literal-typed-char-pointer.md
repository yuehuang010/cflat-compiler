# `u8"..."` literal is typed `char*`, cannot initialize `std::u8string`

C++20: `u8"abc"` is `const char8_t[4]`; `std::u8string(u8"abc")` and `std::u8string s = u8"abc"` are valid. In CFlat
the literal is typed `char*`, so no `std.u8string` constructor matches. Workaround: `std.u8string(3, (char8_t)'a')`.

## Repro

```cflat
import cpp "string";
extern int main()
{
    std.u8string b = std.u8string(u8"xyz");
    return (int)b.size() - 3;
}
```

Actual (verbatim): `C++ class 'std.u8string' has no constructor whose parameter types match these arguments ('char*')`.

## Root cause (GUESS)

The lexer/literal handling treats the `u8` prefix as a plain narrow string; the bridge only spells `char`
literals. (Note: `u8` as a bare identifier is also a lexer prefix, so `u8` cannot name a variable.)

## Fix direction

Type `u8"..."` as `char8_t*` (and spell it as such to clang) like `u"..."`/`U"..."` for u16string/u32string.

Found by: test_libs/std_full/std_full_11_933_u8_literal (2026-10-02).
