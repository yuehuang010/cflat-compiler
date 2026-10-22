# alias return through a user operator-> on a local wrapper is accepted (dangles)

**RULED 2026-10-21:** Refuse: a member operator->/method on a frame-local receiver is frame-rooted for alias returns (extends the 2026-09-24 &ref() gate). Left hold/. See fix-issue-lessons.md digest.

Found in the T274 review (2026-10-15). Pre-existing on master; T274 did not make it worse.

## Repro

```cflat
struct R { int v = 0; };
struct Wr { R r = default; R* operator->() { return &r; } };
alias int f() { Wr w; return w->v; }   // accepted; w (and w.r) die at return
```

## Root cause

The T274 frame-rooted gate recognises core `unique<T>` borrowing methods (`get()`, `operator->`) only.
A user `operator->` returning a pointer into `this` is opaque to it, so the alias return looks like it is
rooted in a call result, not in the local `w`.

## Fix direction / ruling needed

Either treat any member `operator->` (or method) on a frame-local receiver as frame-rooted for alias
returns (may refuse legal wrappers whose operator-> returns a stored heap/global pointer), or leave it
untracked per "raw borrowed-pointer lifetimes untracked". Needs a maintainer ruling before building.
