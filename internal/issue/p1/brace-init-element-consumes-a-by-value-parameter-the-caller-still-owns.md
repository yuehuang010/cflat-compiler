# A brace-init element consumes a by-value owning PARAMETER the caller still owns

Filed 2026-08-09 by the review of `fix/bracown`. `T[N] d = { p };` where `p` is a by-value owning
parameter double-frees; the ASSIGNMENT spelling of the same store, `d[0] = p;`, is clean. Measured
identical on `2f5a91a` (master, pre-`fix/bracown`) and on `fix/bracown`, so this is NOT a regression
of that fix - but it is the one source shape its accept-set matrix missed, and after the fix the
brace path ACTIVELY consumes the parameter, which makes the asymmetry structural rather than an
accidental bit copy.

Severity: double free (abort).

## Repro

```cflat
int dtor = 0;
struct Res { int id = 0; ~Res() { dtor = dtor + 1; } };
struct UBox { unique Res* item = nullptr; };
UBox umk(int n) { UBox b; b.item = new Res(); b.item->id = n; return b; }

void fbrace(UBox p) { UBox[2] d = { p }; }      // BRACE   - rc 134
void fasg(UBox p)   { UBox[2] d; d[0] = p; }    // ASSIGN  - rc 0

extern int main() { UBox a = umk(5); fbrace(a); return 0; }
```

Measured (`scratch/rev_uniq_bp.cb` / `rev_uniq_bp_assign.cb`): the brace form prints the element
value 5, runs the element dtor once at the end of `fbrace`, then aborts on the caller's `a` dtor -
**rc 134**. Swapping the body to `d[0] = p;` (`rev_uniq_bp_assign.cb`) gives rc 0, one free.

## Root cause

The two spellings disagree about whether the CALL SITE knows the parameter was consumed. With the
assignment body, passing `a` to `fasg` marks the caller's `a` moved - a later read of it is rejected
with `use of moved variable 'a'` (measured on `scratch/rev_bp_trace2.cb`, both binaries), and the
caller therefore does not destruct it. With the brace body, the caller's `a` is NOT marked: reading
it after the call compiles and yields garbage, and the caller destructs a resource the callee's
element already freed.

`ConsumeOwningBraceElementSource` (`cflat/MainListener_Expressions.cpp:6614`) accepts a parameter
alloca as `srcIsNamedSlot` and moves out of it - correctly, by the same rule the assignment arm
uses - but the pre-pass that decides a parameter is a consuming (move) parameter does not look at
the fixed-array brace-list lowering, so the two halves disagree. Excluding parameters from the new
arm does NOT fix it: the pre-fix bit copy aborts on exactly the same cell.

## Fix direction

Teach the parameter-consume pre-pass the brace-list element site, so a by-value owning parameter
appearing as a positional fixed-array brace element makes the parameter a move parameter, exactly as
the assignment spelling already does. Verify with the repro above plus the neighbour that must stay
rejected - reading `p` after the brace init already reports `use of moved variable 'p'` on
`fix/bracown` (`scratch/rev_bp_useafter.cb`), matching the assignment spelling, so only the caller
half is missing.
