# The array value-init splat memcpy's ONE owning seed into every element

Filed 2026-08-09 by the review of `fix/fldarr`. The defect lives in the LOCAL declarator's
array value-init arm and is PRE-EXISTING there; `fix/fldarr` newly routes the FIELD position
onto the same emitter, so the field spelling changed from a silent wrong value into this
abort.

Severity: bad code gen - a double free that aborts the program (rc 133).

## Repro

```cflat
string mk() { string a = "he"; string b = "llo"; return a + b; }
struct E { string s = mk(); int v = default; };
struct Holder { E[2] e = { v = 5 }; };            // FIELD spelling
extern int main() {
    Holder h;
    printf("%s %s same=%d\n", h.e[0].s.data(), h.e[1].s.data(),
           (h.e[0].s.data() == h.e[1].s.data()) ? 1 : 0);
    return 0;
}
```

-> compiles rc 0, prints `hello hello same=1`, then aborts at teardown with rc 133.

The LOCAL spelling `E[2] e = { v = 5 };` inside `main` behaves IDENTICALLY, and does so on
BOTH `987ae77` and `d8056c1` - the local arm has always had this.

Measured on the FIELD spelling:

| binary | result |
|--------|--------|
| `987ae77` (pre `fix/fldarr`) | rc 0, prints `  0 0 same=1` - the list was discarded |
| `d8056c1` (`fix/fldarr`)     | rc 0 compile, correct values, **rc 133 abort at teardown** |

`leaks --atExit` reports 0 leaks in both cases; the failure is the second free, not a leak.

Positional lists are NOT affected: `string[2] s = { "ab", "cd" }` as a field, and
`Outer[2] arr = default` over an owner whose fields are brace defaults, both give distinct
buffers, rc 0, 0 leaks (measured).

## Root cause

Both arms build ONE seed element and `CreateMemCpy` it over every slot:

- local: `MainListener_Declarations.cpp` ~2880, the `arrayseed` alloca
- field: `MainListener::EmitFieldDefaultArraySplat` (`MainListener_Expressions.cpp`), the
  `fieldarrayseed` alloca - added by `d8056c1` as a deliberate arm-for-arm mirror of the local
  one

A memcpy is a shallow copy, so every element's `string` field points at the one heap buffer
the seed allocated. Each element is then destructed independently.

## Fix direction

The seed-and-memcpy pattern is only valid for a POD element. Where the element type owns
anything (any field whose destructor runs), the splat has to construct each slot instead -
call the element constructor per slot and re-apply the named overrides, or deep-copy the seed
per slot through the same path `CreateAssignment` uses for an owning struct copy. Fix the
local arm and the field arm together: they are the same algorithm in two places and the field
one was written to mirror it.

Until then, the two spellings agree with each other, which is the property `fix/fldarr` was
aiming for.
