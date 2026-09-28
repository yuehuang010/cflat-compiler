# `p + s` / `p == s` with a pointer LEFT operand and a C++ class right operand is refused

`char* p; std.string s;` - `p + s` and `p == s` (also `<`) do not dispatch to the libc++ free
operator templates (`operator+(const CharT*, const basic_string&)`, `operator==`); clang++
accepts both. A string LITERAL left operand works (`"cd" + s`). Measured on master 2f998b56,
unchanged by the string-plus-char-pointer fix.

## Repro (macOS, libc++)

```cflat
import cpp "string";
extern int main()
{
    std.string s = "ab";
    char* p = "cd";
    std.string t = p + s;
    return t.size() == 4 ? 0 : 1;
}
```

First error: `cannot use a value of type 'std.string' as a pointer arithmetic index; the index
must be an integer` (`p == s`: `no overload of 'operator==' matches operands 'pointer' and
'std.string'`).

## Root cause guess

Additive / equality dispatch only tries TryBinaryOperatorOverload when the LEFT operand is a
struct (or via the literal special case); a pointer left operand goes straight to pointer
arithmetic / pointer compare. Fix direction: when the right operand is a C++ record and the left
is a pointer, try the free-operator path (with the left operand's depth carried, see
`templateArgument` in `tryFreeOperator`) before pointer arithmetic.
