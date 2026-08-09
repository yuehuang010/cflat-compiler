# `o = gOwner` consumes a file-scope global, so the second execution of the same statement gets null

Filed 2026-08-09 by the review of `fix/owncopy`, which made a file-scope GLOBAL owning source take
the transfer arm for the first time (`GetGlobalVariableNV` sets no `CallerName`, so the old
named-slot test never matched one).

```cflat
Box gBox;                       // Box { unique Res* item; } - a NON-copyable owner
int f() { Box o = makeBox(1); o = gBox; return boxId(o); }
extern int main() { gBox = makeBox(5); printf("%d %d\n", f(), f()); return 0; }
```

Measured (`scratch/rev_p_global_twice.cb`): `first=5 second=-1`, rc 0, one free per allocation.
Memory-safe (before `fix/owncopy` this shape was rc 133), but the second call silently reads a
moved-out global. A COPYABLE owner is unaffected - it copies and the global stays live
(`rev_p_global_str_twice.cb`, both calls read the value, `leaks --atExit` clean).

## Why it is filed separately

Program-lifetime storage is never re-initialized before the next execution of the statement, so
"move out of it" has no sound reading. `internal/issue/p2/move-out-of-program-lifetime-storage-crashes-on-reuse.md`
(filed on master by the static-local fix) states the same defect for the EXPLICIT `move` spelling
and recommends REJECTING it. This file records that after `fix/owncopy` a plain `=` reaches the
same storage class implicitly, so whichever reading is chosen there has to cover both spellings.
Merge the two files when the direction is decided.

## Fix direction

Follow whatever `move-out-of-program-lifetime-storage-crashes-on-reuse.md` settles on. Do NOT
narrow the `fix/owncopy` arm back for globals alone: the shape used to double-free (rc 133).
