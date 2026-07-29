# Overload diagnostics: the non-probed replay can report a LOSING candidate's name miss

Filed 2026-07-28 by an adversarial review of the interface named-argument fix.
PRE-EXISTING: `master` produces the identical message. It is the acknowledged design weakness
of recovering a diagnostic by replaying the scoring loop.

Severity: misleading message on an already-failing compile. No miscompile.

## Repro

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

## Root cause

`CreateOverloadedFunctionCall` (`LLVMBackend.h` ~:16660) scores every candidate with
`probe=true` so a losing candidate's name miss cannot hard-error out of the search. When
nothing wins, it replays the same loop with `probe=false` purely to recover the specific
message. The replay has no notion of which candidate the user meant, so the FIRST candidate
that fails on a name wins the diagnostic even when another candidate failed on types.
`ResolveInterfaceMethodSlot` (`LLVMBackend.h` ~:12484) now does the same thing for interface
slots and inherits the same weakness.

## Fix direction

Two parts, in order of value:

1. **Prefer the type failure.** A candidate that BOUND the names but failed to score is a
   better blame target than one that could not bind them. Have the probed pass record, per
   candidate, whether it failed on a NAME or on a TYPE, and only replay-report a name miss
   when no candidate bound the full name set. The generic "no overload matches" dump already
   covers the type case, so this mostly means suppressing the misleading line.

2. **Extract `ScoreCandidates(probe)`.** The probed loop and its non-probed replay are two
   hand-copied loops in `CreateOverloadedFunctionCall`, and now a third pair in
   `ResolveInterfaceMethodSlot`. A future `break`/`continue` added to one and not the other
   would silently desynchronise them - the replay would then diagnose a candidate the real
   search never considered. One helper called twice removes that class of bug outright.
