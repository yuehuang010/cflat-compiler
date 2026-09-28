# Chained C++ `std.string` `+` (`s + a + b`) is refused

A chain of two or more C++ free-operator `+` calls on `std.string` does not compile, whatever the
operands (`s + s + s`, `s + "a" + "b"`, `s + p + p`). clang++ accepts all three. Measured on
master 2f998b56 and unchanged by the string-plus-char-pointer fix.

## Repro (macOS, libc++)

```cflat
import cpp "string";
extern int main()
{
    std.string s = "ab";
    std.string t = s + "cd" + "ef";
    return t.size() == 6 ? 0 : 1;
}
```

First error: `cannot apply binary operator '+' to operands of type 'pointer' and 'pointer':
pointer arithmetic is limited to ...` (`s + s + s`: `cannot use a value of type 'std.string' as a
pointer arithmetic index`).

## Root cause guess

The additive fold (MainListener_Expressions.cpp, the accumulator loop near
`TryBinaryOperatorOverload(accumulator, ...)`) re-enters overload resolution only while the
accumulator `isStructTy()`; the first template operator call returns its sret result as a `ptr`,
so the second `+` falls through to pointer arithmetic. Fix direction: carry the first call's
result as a class value (or its storage + type name) into the next fold step.
