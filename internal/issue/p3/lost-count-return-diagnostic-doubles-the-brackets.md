# The "element count is lost at the return" diagnostic doubles the `[]` in its suggestions

Filed 2026-09-03 while measuring `T[]` behaviour for the hidden-count ruling.

## Repro

```cflat
struct Y { int v = 0; };
Y[] make() { return new Y[3]; }
int main() { return 0; }
```

Prints: `cannot return the heap array the 'new' result as 'Y[]': the element count is lost at
the return. Return 'array<Y[]>' or 'move Y[][]' (which carries the count) instead.`

Expected suggestions: `array<Y>` and `move Y[]`. The element type is spelled with the view's
`[]` suffix still attached, so both suggestions are wrong types. Also "the heap array the 'new'
result" reads as two subjects glued together.

## Fix direction

At the LogError site (grep "element count is lost at the return" in cflat/), spell the ELEMENT
type (strip the `[]` / `*` declarator) for both suggestions and reword the subject to "the
heap array from 'new'". Regression: an `expect_error` leg on the corrected substring in an
existing `Test/errors/err_*` file that already covers this rule (grep for the message).
