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

A THIRD spelling reaches the same defect after `fix/bracown`: a struct FIELD DEFAULT whose brace
list names a global owner, consumed at DEFAULT CONSTRUCTION.

```cflat
Box fdSeed;
struct FdWrap { Box[2] arr = { fdSeed }; };
extern int main()
{
    fdSeed = mk(5);
    FdWrap w1 = default;   // w1.arr[0] = 5, fdSeed nulled
    FdWrap w2 = default;   // w2.arr[0] is NULL
    return 0;
}
```

Measured (`scratch/rev_fielddefault2.cb`): `w1=5 seed_null=1 w2=-1`, rc 0, `leaks --atExit` clean;
rc 134 on `2f5a91a`, so this too is memory-safety gained, value silently lost. This spelling is the
worst-reading of the three, because a field default is meant to be a per-instance INITIALIZER and
is written once for every construction of the type - a reader has no cue that it is one-shot. A
COPYABLE owner is unaffected here as well (`scratch/rev_fd_copyable.cb`: both instances get 5, the
global stays live). `Test/test_move.cb`'s `abri_fielddefault_*` legs depend on being the file's only
construction of `OaiFdWrap`; a second one would read a nulled seed and the legs would go vacuous.
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
