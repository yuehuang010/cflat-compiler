# An integer literal is typed as the SMALLEST fitting type, so overload resolution picks `i8`

Filed 2026-08-21, found while triaging an external report (the `[1] i8` in the v0.11.0 issue 02
diagnostic for `string_view(buf, 5)`). Reproduced on `39d4b38`.

## Repro

```cflat
void f(i32 x) { printf("i32 %d\n", x); }
void f(i8 x)  { printf("i8 %d\n", x); }
extern int main() { f(5); f(300); return 0; }
```

Measured output:

```
i8 5
i32 300
```

So the literal's type is chosen by its VALUE, not fixed at `int`. C, C++, and Rust all type an
unsuffixed decimal literal as `int`/`i32` and would select the `i32` overload in both calls.

## Why it matters

1. **Overload selection changes when a constant changes.** Editing `f(5)` to `f(300)` silently
   switches which function runs. That is a footgun in any library that overloads on width.
2. **Confusing diagnostics.** An unmatched call reports `[1] i8 <unnamed>` for what the user wrote
   as `5`, which reads like a compiler bug (it is what prompted the external reporter to flag it).
3. It is not documented. `doc/LANGUAGE.md` says nothing about literal typing beyond the `0`->null
   pointer rule.

## Fix direction

Decide and then write it down - this may well be a deliberate choice that just needs documenting:

- **Option A (align with C/C++/Rust):** an unsuffixed integer literal is `i32`; it converts
  implicitly to a narrower type only in a context that demands one (assignment/initialization to a
  narrower variable, or as the sole viable overload). Overload resolution then prefers `i32`.
  Behaviour change - check the suite for tests that rely on today's rule.
- **Option B (keep, document):** document "smallest fitting type" in `doc/LANGUAGE.md`, and make
  the overload diagnostic print the literal as written (`5` typed as `i8`) so it does not look
  like the compiler invented an `i8`.

Option A is the lower-surprise answer; Option B is nearly free. Do not leave it undocumented.

## Adjacent

- [[string-view-no-ptr-len-constructor]] - where this surfaced.
