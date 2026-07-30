# Overload/slot diagnostics: the non-probed replay blames the wrong candidate

Consolidated 2026-07-30 from `named-arg-replay-reports-losing-candidate` (filed 2026-07-28 by
an adversarial review of the interface named-argument fix) and
`iface-slot-replay-blames-wrong-slot` (filed 2026-07-28 by that fix's round-2 review). Both
files independently named the same root and the same fix; they are merged here rather than
cross-linked because neither can be fixed alone without duplicating the other's work.

PRE-EXISTING for the free-function half: `master` produces the identical message. It is the
acknowledged design weakness of recovering a diagnostic by replaying the scoring loop.

Severity: **misleading message, factually FALSE, on two paths.** The free-function path is
message-only. The interface-slot path is worse - see "Why the slot half is worse" - because
its replay sits in front of a fallback that exists to let a merely-unranked call through, so
a wrong message there converts a success into a failure.

## The shared root

Both `CreateOverloadedFunctionCall` (`LLVMBackend.h` ~:16660) and `ResolveInterfaceMethodSlot`
(`LLVMBackend.h` ~:12484) score every candidate with `probe=true`, so a losing candidate's
name miss cannot hard-error out of the search. When nothing wins, each replays the same loop
with `probe=false` purely to recover a specific message. **The replay has no notion of which
candidate the user meant**, so the FIRST candidate that fails on a name wins the diagnostic
even when another candidate failed on types - or, on the slot path, even when the names were
meant for a different slot entirely.

That is one mechanism in two hand-copied loop pairs (three, counting the free-function pair
itself).

## Repro A - free function, losing candidate reported

`scratch/adv25_misleading_msg.cb`:

```cflat
int h(int alpha) { return alpha; }
int h(const char* beta) { return 1; }

extern int main()
{
    printf("%d\n", h(beta: 5));
    return 0;
}
```

Reports:

```
named argument 'beta' does not match any parameter
```

which is false - `beta` is exactly the parameter of overload 2. The real failure is that
`int` does not convert to `const char*`. Overload 1 (`alpha`) is the candidate that
name-misses, and it is reported because it is replayed first.

## Repro B - interface slot, wrong slot's names reported

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

`b` IS declared - on slot 0, the slot the call names. The diagnostic comes from slot 1's
replay, whose parameters are `p`/`q`. A weaker form (names valid only for slot 1) fails the
same way.

`ResolveInterfaceMethodSlot` (`cflat/LLVMBackend.h:12477-12480`) replays EVERY `byArity`
slot non-probed once no slot ranked, so any slot whose parameter names differ from the
call's names errors regardless of which slot the names were meant for.

The in-code justification at `:12479` - "An all-positional call cannot fail here: arity
already matches, so pass 2 always binds" - is TRUE and was verified, but it only covers
POSITIONAL calls. It does not cover the named calls the replay exists to serve.

## Why the slot half is worse

In `CreateOverloadedFunctionCall` the replay precedes a call that was going to fail anyway,
so a wrong message is only a wrong message. In `ResolveInterfaceMethodSlot` the replay
precedes the historical `return byArity[0]` fallback, which exists precisely to let a
program the scorer merely fails to RANK through - so the replay converts a success into a
failure.

Mitigating: reaching the fallback means no slot ranked, so the argument types are wrong for
the slot the user named. Rejection is directionally right, and it is strictly safer than the
prior behaviour, where this shape compiled and mis-ordered the arguments silently. Only the
message is wrong.

## Why the test corpus does not catch the slot half

`INArgUnrank` in `Test/test_interface.cb` gives BOTH slots the same parameter names
(`p`, `q`), so the replay is silent there. A regression test needs slots whose names DIFFER.

## Fix direction

Three parts, in order of value. Parts 1 and 2 are the point fixes for each half; part 3 is
the durable fix that subsumes both and is the reason these are one issue.

1. **Free-function half - prefer the type failure.** A candidate that BOUND the names but
   failed to score is a better blame target than one that could not bind them. Have the
   probed pass record, per candidate, whether it failed on a NAME or on a TYPE, and only
   replay-report a name miss when no candidate bound the full name set. The generic "no
   overload matches" dump already covers the type case, so this mostly means suppressing the
   misleading line.

2. **Slot half - gate the replay on `candidates.empty()`**, i.e. nothing bound by name at
   all, rather than running it unconditionally. When some slot did bind, keep the historical
   pick and take the permutation from that slot.

3. **Extract one `ScoreCandidates(probe)` helper, called twice**, and report the
   best-scoring candidate's real failure rather than the first candidate's. Three
   hand-copied loop pairs exist today; a future `break`/`continue` added to one and not its
   twin would silently desynchronise them, and the replay would then diagnose a candidate
   the real search never considered. One helper removes that class of bug outright.

## Related

[[interface-issue-queue]]
