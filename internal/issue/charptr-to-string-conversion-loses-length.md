# Implicit `char*` -> `string` conversion produces a string of length 0

Filed 2026-07-24, found while reviewing the ternary branching fix. Pre-existing, unrelated to
that work, and reproducible with no ternary involved. Verified against master `9f967de`.

## Summary

Assigning a `char*` to a `string` yields a `string` whose length field is 0, even though the
pointer is valid and the bytes are present. Any length-based operation on the result then
silently misbehaves - `length()` returns 0, and since `string` comparison, `compare()`, and
`operator<` are all length-based (see `cflat/core/string.cb:254-261`), they all give wrong
answers too.

## Repro (verified, exit 0, wrong output)

```cflat
import "string.cb";
struct Token { string text = default; int id = default; };
Token makeToken(string t) { Token k = default; k.text = "" + t; k.id = 7; return k; }
extern int main()
{
    Token k = makeToken("hello");
    printf("direct len=%d\n", (int)k.text.length());   // 5   - correct
    string s = k.text.data;                            // char* -> string
    printf("via data len=%d\n", (int)s.length());      // 0   - WRONG, should be 5
    return 0;
}
```

Output:
```
direct len=5
via data len=0
```

No diagnostic, exit 0. The pointer itself is fine; only the length is lost.

## Scope

Reproduces for a `char*` from any source, not just a struct field - a `char*` local, a `char*`
returned from a function, and a heap `char*` all behave the same. It is the conversion that is
wrong, not the provenance.

Note the correct spelling for this particular example is just `string s = k.text;` (a real copy),
so the shape above is somewhat unusual on its own. But the conversion is reachable from ordinary
C-interop code, where a `char*` coming back from a C function is the normal thing to have.

## Root cause

Not investigated. The `char*` -> `string` conversion builds the two-field `%string`
(`{ ptr, i32 }`) with the pointer set and the length left at 0 rather than calling `strlen` (or
otherwise deriving the byte count). Look at wherever a pointer operand is wrapped into a `string`
during assignment/type unification.

## Fix direction

Derive the length at the conversion site (a `strlen` call for a NUL-terminated `char*`), or - if
the length genuinely cannot be known - reject the implicit conversion with a `LogError` telling
the user to construct the `string` explicitly. Silently producing a zero-length string is the one
option that should not survive, because it fails later and far from the cause.

Regression test: extend `Test/test_string.cb` (or the nearest existing string test) with a leg
asserting `string s = someCharPtr;` has the expected length.

## Related, now fixed

The ternary spelling of the same conversion used to crash the compiler outright:

```cflat
string s = c ? makeToken("h").text.data : makeStr("x");   // SIGSEGV, exit 139, no output
```

That crash was on `9f967de` and is fixed incidentally by the `?:` branching change (it now
produces the same wrong-length value as the non-ternary form above, i.e. it degrades to this
issue rather than crashing). Do not re-file the crash; fixing the conversion closes both.
