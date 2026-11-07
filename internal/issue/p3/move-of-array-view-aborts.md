# `int[] b = move a;` of a brace-initialized view aborts at run time

## Summary

Moving an array view into another view and then iterating both compiles and aborts (exit 134). Found by the brview
round-2 review (box 10, 2026-11-03), PRE-EXISTING (master and the brview branch alike).

## Minimal repro

```cflat
extern int main() { int[] a = {1, 2, 3}; int[] b = move a; int n = 0; for (int x in a) n++; for (int x in b) n += 10; printf("%d\n", n); return 0; }
```

RULED 2026-11-04 (maintainer): `move` of an array view into a LOCAL view transfers the source's own bit (`b.owns =
a.owns`, the path a reassigned view already takes): a non-owning source is a plain copy, source nulled, no adoption. Into a
`move T[]` SINK, a proven frame-backed view (brace-initialized, bound to a local fixed array) is REFUSED - 2026-09-03
slot-move rule ("move from the owning slot"); the sink ABI carries the count, not the own bit. Supersedes the same-day
main-session refusal of every frame-backed move. Test consequence: the brace and fixed-array local shapes become value legs
(30, rc 0); one new Test/errors/err_ leg for the sink shape.

Root cause (IR, f5a491f7): `a.owns` is false, but the decl-init move path stores `b.adopt = true` unconditionally, so b
deletes the stack backing at scope exit (rc 134); a view reassigned anywhere takes the `moved_view_owns` path and is correct.
Probes scratch/repro_keep/moveview/: mv1 brace + mv3 fixed-array print 30 then rc 134; mv2 heap view 30 rc 0; mv4 borrowed
param moved, called twice, correct; mv5 brace view into `sink(move int[])` rc 134; mv6 conditionally owning view 3 rc 0.

Observed (master c3d63f2f): prints 0, then exit 134. Expected: either `move` of a view is refused (a view does not own its
elements) or the moved-from view reads as empty and b iterates 3 times (prints 30) with no abort - needs a ruling on
what `move` means for a non-owning view; check what the abort is first (lldb).
