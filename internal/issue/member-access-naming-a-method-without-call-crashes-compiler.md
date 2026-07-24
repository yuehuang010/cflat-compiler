# Member access that names a METHOD without call parens segfaults the compiler

Filed 2026-07-24, found while reviewing the ternary branching fix. Pre-existing and unrelated
to that work. Verified against master `9f967de`.

## Summary

Writing `obj.method` (no parentheses) - a member access that resolves to a method rather than a
field - crashes the compiler with SIGSEGV (exit 139) instead of producing a diagnostic. It
affects core types and user-defined structs alike, and crashes on every driver path tried:
`--check`, `-o`, and `--out-lli`.

## Repro 1 - user-defined struct (minimal)

```cflat
struct P { int v = 0;  int getv() { return this->v; } };
extern int main()
{
    P p = default;
    printf("%d\n", (int)p.getv);      // note: no '()' - names the method
    return 0;
}
```

```
x64/Release/cflat scratch/g.cb --check     -> exit 139 (SIGSEGV, no output)
x64/Release/cflat scratch/g.cb -o g.exe    -> exit 139
```

## Repro 2 - core `string` (how it was found)

```cflat
import "string.cb";
extern int main()
{
    string t = "lit";
    printf("%d\n", (int)t.length);    // 'length' is a METHOD, not a field
    return 0;
}
```
Same crash on all three driver paths.

`cflat/core/string.cb:36` declares `i32 length(string self)` - a method. The correct spelling is
`t.length()`, which compiles and runs fine. So the user's mistake here is a plain missing `()`,
which is an extremely easy typo to make and currently costs them a compiler crash with no
message, no source location, and no indication of which line is at fault.

## Why this matters

CLAUDE.md's standing rule: "When encountering a LLVM assert, after identifying the root cause,
then write an proper error message in the compiler to avoid that case." This is worse than an
LLVM assert - it is a raw SIGSEGV with no diagnostic at all.

Note it crashes under `--check`, which is the LSP/IDE path. An editor that runs `--check` on
each keystroke will hit this the moment a user types `t.length` before typing `(`, i.e. during
perfectly ordinary editing of correct code. That makes it more than a bad-input diagnostic gap.

## Root cause

Not investigated. The crash is in member-access resolution: the access finds a method in the
member lookup and then proceeds down the field path (reading a null/absent field descriptor)
rather than either rejecting the expression or forming a bound-method value. Start at the
member-access handling in `MainListener.h` (the `->`/`.` postfix path) and look at what happens
when the resolved member is a function rather than a data member.

Worth checking whether the same shape crashes for an INTERFACE method and for a static/namespace
member, since those go through different lookup paths.

## Fix direction

Emit a `LogError` naming the actual problem, e.g.:

> 'length' is a method of 'string'; did you mean to call it? Write 't.length()'.

If CFlat ever wants method values (bound method references assignable to a `function<>`), that is
a separate feature - but the crash must be replaced by a diagnostic either way.

Regression test: add an `expect_error` leg to an existing `Test/errors/err_*.cb` covering both the
user-struct and the core-`string` spellings. Do NOT create a new test file.
