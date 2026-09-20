p3

# A CFlat character literal is spelled `int` at a C++ template boundary

## Summary

`std.string a; a + 'x'` does not compile, while `a + c` for a `char c` and `a + (char)33` both do
(fixed 2026-09-19 with the `char`/`long`/`wchar` identity plumb). A character literal reaches the
generated-wrapper argument path as a plain integer CONSTANT carrying no type name, and the
constant branch of the spelling lambdas ranks every integer literal as `int`:

    type = llvm::dyn_cast_or_null<llvm::ConstantInt>(arg.Primary) != nullptr
        ? (bits > 32 ? "i64" : "int") : ...

(`LLVMBackend_CInterop.cpp`, the `cflatTypeOf` / `argumentType` lambdas).

## Repro

```cflat
import cpp "string" cache;
extern int main()
{
    std.string a = std.string("hi");
    std.string t = a + 'x';           // p.cb(5,19): no operator '+' for type 'std.string'
    return (int)t.size();
}
```

`cppsop.typeCodeOf('x')` (Test/library/cpp_interop_strop.h) returns 7 (`int`), not 1 (`char`).

## Root cause

Two things, both deliberate today:

- `MainListener_PostfixExpression.cpp` builds a character literal through
  `CreateConstant(ConstantVariant(c))`, which yields a bare `llvm::Value` with no source type
  name, so nothing downstream can say the constant was written as a character.
- the literal-ranks-as-int rule above exists so an unsuffixed integer literal scores like a C++
  `int` in overload resolution. Spelling a character literal `char` means exempting it from that
  rule, which moves the ranking of every call that passes one.

## Needs a ruling

C says `'x'` is `int`; C++ says `char`; CFlat's own overload resolution already prefers
`f(char)` over `f(int)` for `f('x')` (measured 2026-09-19). The primitive-identity ruling
(`internal/fix-issue-lessons.md`, 2026-09-10) says identity follows C++, which points at `char`,
but the change has to name what happens to `L'x'` / `u'x'` / `U'x'` and to the int-ranking rule.

## Acceptance

`a + 'x'` concatenates and `cppsop.typeCodeOf('x') == 1`, with `cppsop.typeCodeOf(5) == 7`
unchanged, and `Test/test_cpp_interop.cb` / `Test/test_cpp_interop_template.cb` still green.
