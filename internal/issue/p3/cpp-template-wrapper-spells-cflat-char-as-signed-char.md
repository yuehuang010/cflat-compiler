p3

# A C++ template wrapper spells a CFlat `char` argument as `signed char`

## Summary

`RequestCxxFunctionTemplate`'s `cflatTypeOf` derives an argument's C++ spelling from the LLVM
value when the NamedVariable carries no type name, and maps every 8-bit integer to `i8`, which
`CxxSpellingForCflatType` spells `signed char`. A CFlat `char` is therefore offered to C++
template argument deduction as `signed char`, a distinct type from `char`.

## Repro

```cflat
import cpp "string" cache;
extern int main()
{
    std.string a = std.string("hi");
    char c = 33;
    std.string t = a + c;     // and the reversed order, c + a
    return (int)t.size();
}
```

```text
p_plus_charvar.cb(2,74): no operator '+' for type 'std.string'
```

The generated wrapper's refusal is
`clang: invalid operands to binary expression ('std::string' and 'signed char')`.
libc++ declares `operator+(const basic_string<_CharT, _Traits, _Alloc>&, _CharT)`: `_CharT`
deduces to `char` from the first parameter and to `signed char` from the second, so deduction
fails. `(char)33` behaves the same - the literal reaches the builder as an i8 constant.

## Root cause

The operand's SOURCE type name is not plumbed into the wrapper builder. `cflatTypeOf` only sees
`llvm::Type*` for a scalar, and `i8` is ambiguous between CFlat `char` and a raw byte.

This is NOT a general primitive-identity defect: a CFlat `char` binds a C++ `char` parameter
correctly through the ordinary member-call path. Measured on master 884a0c08 and on the
free-operator-template branch, `cppsop.CharParam::operator+=(char)` accepts a CFlat `char` local
and returns 1 on both.

## Fix direction

Carry the operand's `TypeAndValue.TypeName` into the argument the wrapper builder sees (the
binary-operator path in `MainListener_Expressions.cpp` builds its own NamedVariables and has the
name available), or give `cflatTypeOf` a way to tell a CFlat `char` from a raw i8. Needs a
ruling on which C++ spelling a CFlat `char` is, since `signed char` is deliberate elsewhere.

## Acceptance

`std.string + char` and `char + std.string` both compile and concatenate, in both orders, for a
`char` local and for a `(char)` cast literal, with the result's size and bytes asserted.
