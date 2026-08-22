# `?.` method call on a `string?` STRUCT FIELD segfaults at runtime

Filed 2026-08-21 while working the p3 string-from-a-temporary issue. Pre-existing on `031aefe`
(master) - NOT introduced by that work, and unrelated to containers or temporaries.

## Repro

```cflat
import "string.cb";
struct NBar { string? date = default; };
extern int main() {
    NBar n0 = default;
    n0.date = "nn";
    printf("L=%d\n", (int)n0.date?.length());   // SIGSEGV, exit 139
    return 0;
}
```

`--check` passes and codegen succeeds; the crash is at RUNTIME (exit 139, no output).

## What still works

- `NBar n0 = default; n0.date = "nn";` on its own - fine.
- `string? s = n0.date;` and a `s == nullptr` test - fine.
- The same `?.` call on a `string?` LOCAL - not measured, worth checking first.

So the failing ingredient is the null-conditional CALL on a nullable-string FIELD, not the
nullable string field itself.

## Fix direction

Look at the `?.` lowering for a nullable-string receiver reached through a struct-field GEP:
the null test is probably applied to the wrong operand (the field's address, which is never
null) or the receiver is passed as the raw `{ptr,len}` aggregate where the callee expects a
pointer. Dump `--out-lli` for the repro and compare with the `string?` LOCAL spelling.
