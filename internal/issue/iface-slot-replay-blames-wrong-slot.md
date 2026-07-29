# Interface slot replay blames the wrong slot's parameter names

Filed 2026-07-28 by the round-2 review of the interface named-arguments fix.

Severity: degraded diagnostic (the message is factually FALSE) plus a
silent-accept-to-reject behaviour change on programs that had no correct binding
anyway. Narrow.

Sibling of [[named-arg-replay-reports-losing-candidate]] - same root shape, and the
two should be fixed together. See "Fix direction" below.

## Repro

```cflat
interface IF
{
    int f(A* a, A* b);
    int f(B* p, B* q);
};
// ... io is an IF, c1/c2 are C* (matching neither slot)
io.f(b: c1, a: c2);
```

```
repro.cb(26,36): named argument 'b' does not match any parameter
```

`b` IS declared - on slot 0, the slot the call names. The diagnostic comes from slot
1's replay, whose parameters are `p`/`q`.

A weaker form (names valid only for slot 1) fails the same way.

## Root cause

`ResolveInterfaceMethodSlot` (`cflat/LLVMBackend.h:12477-12480`) replays EVERY
`byArity` slot non-probed once no slot ranked. Any slot whose parameter names differ
from the call's names therefore errors, regardless of which slot the names were
meant for - the first one to fail wins the message.

The in-code justification at `:12479` - "An all-positional call cannot fail here:
arity already matches, so pass 2 always binds" - is TRUE and was verified, but it
only covers POSITIONAL calls. It does not cover the named calls the replay exists
to serve.

## Why this is worse than its sibling

In `CreateOverloadedFunctionCall` the replay precedes a call that was going to fail
anyway, so a wrong message is only a wrong message. HERE the replay precedes the
historical `return byArity[0]` fallback, which exists precisely to let a program the
scorer merely fails to RANK through - so the replay converts a success into a
failure.

Mitigating: reaching the fallback means no slot ranked, so the argument types are
wrong for the slot the user named. Rejection is directionally right, and it is
strictly safer than the prior behaviour, where this shape compiled and mis-ordered
the arguments silently. Only the message is wrong.

## Why the test corpus does not catch it

`INArgUnrank` in `Test/test_interface.cb` gives BOTH slots the same parameter names
(`p`, `q`), so the replay is silent there. A regression test for this needs slots
whose names DIFFER.

## Fix direction

Gate the replay on `candidates.empty()` - nothing bound by name at all - rather than
running it unconditionally. When some slot did bind, keep the historical pick and
take the permutation from that slot.

The durable fix covers both this and [[named-arg-replay-reports-losing-candidate]]:
extract one `ScoreCandidates(probe)` helper called twice, and report the
best-scoring candidate's real failure rather than the first candidate's. That also
removes the desync hazard of two hand-maintained replay loop pairs, where a future
`break` or `continue` added to a scoring loop would silently diverge from its replay.

## Related

[[named-arg-replay-reports-losing-candidate]], [[interface-issue-queue]]
