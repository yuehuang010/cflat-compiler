# No `string_view(ptr, len)` / `string(ptr, len)` constructor - consuming a `(char*, int)` callback
# needs a per-char loop

Filed 2026-08-21 from an external report (v0.11.0 issue 02). Reproduced on `39d4b38`.

## Repro

```cflat
import "string.cb";
extern int main() {
    char* buf = "hello world";
    string_view v = string_view(buf, 5);
    printf("%d\n", v.length());
    return 0;
}
```

```
no overload of 'string_view' matches the given arguments.
  Call arguments (2):
    [0] ptr* <unnamed>
    [1] i8 <unnamed>
  Candidates (1):
    _string_view_string_view__()
```

`string_view` (`cflat/core/string.cb:593`) is `{ i8* _ptr; i32 _len; }` with only a default
constructor. The only ways to build one are `string.view()` and `string.span(start, end)`, both of
which require an existing `string` - so a raw `(char*, int)` chunk from a C callback (`process`
stdout, a socket read, a C library) has no direct route in. The reporter's workaround was an
`appendChar` loop per byte.

## Fix direction

Add to `cflat/core/string.cb`:
- `string_view(i8* ptr, i32 len)` - trivial, just fills the two fields. Borrowing semantics are
  already what `string_view` means, so no ownership question arises.
- `string(i8* ptr, i32 len)` - copies `len` bytes and NUL-terminates; the owning counterpart.

Both are the standard C++ `string_view(ptr, count)` / `string(ptr, count)` spelling, so the shape
is uncontroversial.

## Note

The `[1] i8` in the diagnostic above is not a typo - see
[[integer-literal-typed-as-smallest-fitting-type]]. It is a separate issue and would make a
hand-written `(i8*, i32)` overload fail to match a literal length argument unless implicit widening
covers it; check that when adding the constructor.
