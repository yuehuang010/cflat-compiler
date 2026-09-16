# No functional-construction spelling for the builtin `string`

`string` is a hard keyword, so it cannot appear in expression position. `string_view` is an
ordinary struct and constructs the way the docs show; `string` has no counterpart.

## Repro

```cflat
import "string.cb";
extern int main()
{
    char[4] buf = default;
    buf[0] = 'x'; buf[1] = (char)0;
    string_view v = string_view(&buf[0], 1);   // OK - documented
    string c = string(&buf[0]);                // error
    string d = (string)&buf[0];                // OK - cast form works
    return 0;
}
```

```
(7,15): error: unexpected 'string' here; expected {'alignof', 'simd', 'default', 'sizeof',
'nameof', 'typeof', 'iidof', 'winrtDelegate', 'new', 'delete', 'move', 'operator', '(',
'{', '<', '+', '-', '*', '&', '!', '~', Constant, Identifier, DigitSequence, StringLiteral}
```

The diagnostic does not mention the working spelling.

## Proposed spelling

Allow `string` in a functional-cast position, with the two arities `string_view` already
has:

- `string(ptr)` - equivalent to today's `(string)ptr`, a BORROW of a NUL-terminated buffer.
- `string(ptr, len)` - borrow of `len` bytes, the `string_view(ptr, len)` shape.

Both stay borrows, matching the existing implicit `const char*` -> `string` wrap; an owned
result is still `.copy()`.

## Alternatives considered

1. **Do nothing.** `(string)ptr` already works and covers arity 1. But there is no spelling
   at all for the length-carrying form without going through `string_view(p, n).toString()`,
   which allocates.
2. **`string.from(ptr, len)` / a free `make_string(ptr, len)` helper in `core/string.cb`.**
   No grammar change, but it makes the builtin read differently from every library type.
3. **Diagnostic only** - keep the rejection, but say "`string` cannot be called; use
   `(string)expr` or `string_view(ptr, len)`". Cheapest, and worth doing regardless of
   whether the construction form lands.

## Acceptance

- `string(p)` and `string(p, n)` compile and produce the same borrow `(string)p` and
  `string_view(p, n)` do; `sizeof` and `.length()` agree.
- The cast form `(string)p` keeps working unchanged.
- The rejection diagnostic, in whatever form survives, names a working spelling.
- Coverage extends an existing `Test/test_string*.cb`, no new test file.

## Needs a maintainer ruling on the surface before build.
