# p4: owning `string?` (ruling needed)

Moved from p3 2026-08-23. Not a bug: `T?` is a nullable pointer by definition; `string?` holding
text is a convenience the type system does not offer yet.

- Proposed: option 2 below - `string?` becomes an owning `string` plus a present bit, so
  `string? s = "ab" + "cd";` and `string? t = nullptr;` both work and `s?.length()` reads as today.
- Alternative: option 1 below - document `string?` as a borrowed pointer and recommend an empty
  `string` for "no text"; zero compiler work, ergonomics unchanged.
- Acceptance (if option 2): assignment from a string temp, `nullptr`, and another `string?`;
  `?.` / `??` over it; destructor runs once; `--init` round-trip if TypeAndValue gains a field;
  legs in the closest existing nullable test.

---

# `string?` can only be null or `&someStringLocal` - there is no owning optional string

Filed 2026-08-21 alongside the fix that closed the `string?` SIGSEGV. This is the RESIDUAL of
that fix, not a regression: the crash it replaced was a silent use of character bytes as a
`string` struct.

`T?` sets `Pointer` (MainListener_Declarations.cpp ~872), so `string?` is exactly `string*` plus
a null marker. The three things a user would write are now:

```cflat
string? a = nullptr;        // ok
string  t = "ab" + "cd";
string? b = &t;             // ok - borrows t, and `b?.length()` is 4
string? c = "nn";           // now an ERROR (used to compile and then SIGSEGV on any member read)
string? d = t;              // error: "the right-hand side must be a pointer"
```

So a `string?` FIELD can only ever borrow a `string` that outlives the struct. There is no
spelling for "this struct optionally owns some text", which is what `struct Row { string? date; }`
reads like it means.

## Fix direction

A language decision, not a bug fix. Either:

1. Document `T?` as strictly "nullable pointer to T" and leave `string?` as a borrow (then the
   ergonomic answer for optional text is an empty `string`, which already round-trips), or
2. Give `string?` its own representation - an owning `string` plus a present bit - so
   `string? d = t;` and `string? c = "nn";` mean what they look like. That is a new type, and it
   has to answer copy/move, `??`, and the `?.` merge type, so it needs a plan file first.

Whichever is chosen, `doc/LANGUAGE.md` says nothing about `T?` today beyond `?.`; the ruling
belongs there.
