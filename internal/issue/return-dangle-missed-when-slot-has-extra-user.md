# Return-dangle check is silently skipped when the returned slot has any extra user

Residue of the fix that closed `interface-return-dangle-defeated-by-intermediate-local`.
Not a regression: these shapes were ALL accepted before that fix too. This records the
shapes it still misses, and why widening it is the wrong move.

Severity: missed dangle, no diagnostic. Same class as the pre-fix behaviour.

## Repro

Two programs with identical semantics, both of which dangle:

```cflat
interface IShape { int area(); };
class Square : IShape { int s = 0; int area() { return s * s; } };
int measure(IShape s) { return s.area(); }

// REJECTED (correctly) - the slot's only users are the frame-box store and the return load.
IShape viaCall(int n) { Square loc; loc.s = n; IShape r = loc as IShape; measure(r); return r; }

// ACCEPTED, still dangles - `r.area()` dispatches through the slot, producing an extra
// user, which the pass reads as accept evidence and stops on.
IShape viaMethod(int n) { Square loc; loc.s = n; IShape r = loc as IShape; printf("%d\n", r.area()); return r; }
```

A null store has the same effect, deliberately:

```cflat
IShape nulled(int n) { Square loc; IShape r = loc as IShape; if (n < 0) { r = nullptr; } return r; }
```

## Root cause

`RunInterfaceReturnDangleCheck` (`LLVMBackend.h`) rejects only when EVERY user of the
returned local's slot is recognized and at least one is a ledger-confirmed `FrameStorage`
box. Any user it does not explicitly whitelist - a method dispatch through the slot, a
call argument, a GEP with a non-load user, a memcpy, a null store - is ACCEPT evidence and
stops the walk. So whether the dangle is caught depends on an IR-shape accident that is
invisible in the source: `measure(r)` consumes the loaded fat value and leaves the slot
with no extra user, while `r.area()` does not.

## Fix direction - do NOT widen the rule

This polarity is load-bearing and was arrived at after three abandoned attempts that all
REJECTED LEGAL PROGRAMS. The governing asymmetry for this whole family is that a false
rejection is a blocker while a missed dangle is merely today's behaviour, so every
unrecognized shape must land on accept. Reclassifying any of the above as neutral -
"a method call does not write the slot, so it is safe to ignore" - is exactly the
reasoning that produced the earlier false rejections, because the analysis then has to
prove a negative about a user it does not fully model.

The durable fix is not a bigger whitelist. It is front-end provenance: record at the
BINDING site that an interface local was initialized from frame storage, and carry that
on the `NamedVariable` rather than recovering it from the finished IR. Note that a
source-level "tainted binding" property was explicitly rejected for the ORIGINAL issue -
see the deleted issue file's history in the queue - because a missed assignment site
produces a false rejection. Any attempt here must solve that first: it needs every
assignment site to interface locals to be observable, which
[[interface-boxing-sites-not-fully-consolidated]] is the prerequisite for.

Until then this is a known, bounded gap and should stay filed rather than patched.

Related: [[interface-issue-queue]], [[interface-boxing-guards-are-binding-dependent]].
