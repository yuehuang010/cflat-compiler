# An integer literal is typed as the SMALLEST fitting type, so overload resolution picks `i8`

Filed 2026-08-21. Re-measured 2026-08-21 on the p3 bundle branch (off `819848e`): UNCHANGED, and
deliberately left out of that bundle because closing it needs a ruling, not a fix.

## Repro (re-measured, still exact)

```cflat
void f(i32 x) { printf("i32 %d\n", x); }
void f(i8 x)  { printf("i8 %d\n", x); }
extern int main() { f(5); f(300); return 0; }
```

```
i8 5
i32 300
```

## What the bundle established

- Implicit widening DOES cover the narrow-literal-into-wider-parameter case: the new
  `string_view(i8* ptr, i32 len)` constructor in `cflat/core/string.cb` matches
  `string_view(buf, 5)` even though the literal `5` is typed `i8`. So the rule costs nothing
  when only ONE candidate exists; it only bites overload SETS that overload on width.
- The confusing `[1] i8 <unnamed>` line in the unmatched-call dump is still what the user sees
  for a literal they wrote as `5`.

## The ruling still needed (unchanged from the filing)

- **Option A (align with C/C++/Rust):** an unsuffixed integer literal is `i32`, narrowing only in
  a context that demands it. Behaviour change - the suite has legs that pass a literal into
  narrow slots (e.g. `Test/test_operators.cb` u8/i8 legs), so this needs its own measured pass.
- **Option B (keep, document):** document "smallest fitting type" in `doc/LANGUAGE.md` and make
  the overload diagnostic print the literal as written instead of its inferred type.

Do not close this by documenting silently: pick A or B explicitly.

## Adjacent

- `string-view-no-ptr-len-constructor` (fixed in the p3 bundle) is where this surfaced.
