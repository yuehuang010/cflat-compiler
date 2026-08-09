# Consider a diagnostic for a program 'run' differing from the reserved slot only by 'move'

## Summary

A `program P` may declare `bool run(list<string> args)` (no `move`). This is a legal,
distinct overload slot - the reserved synthesized slot is `bool run(move list<string>)`,
and RejectIfProgramMemberSlotTaken deliberately does not cover the move-less spelling.
Calling it calls the user's method and does NOT start the program thread; since the
join-null-guard fix, `P p; p.run(b); p.WaitForExit();` is a silent no-op whose only tell
is `exitCode` staying at -1 (previously it segfaulted rc 139).

That is correct by the overload rules, but a user who wrote the move-less spelling almost
certainly MEANT the reserved slot and will not understand why their program never runs.

## Suggestion

A diagnostic (or the fallback rejection the original p1 issue named) when a `program`
declares a `run` whose signature differs from the reserved `bool run(move list<string>)`
ONLY by the `move` qualifier. Different arity, return type, or parameter types stay
accepted silently - those legs are pinned in Test/test_program.cb (ReservedName* and
testReservedNameMovelessRun).

Filed 2026-08-09 from the review of the join-null-guard fix (see
p2/waitforexit-stop-token-hangs-on-unstarted-program.md for the sibling residue).
