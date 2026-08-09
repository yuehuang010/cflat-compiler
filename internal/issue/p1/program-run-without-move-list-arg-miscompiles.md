# A program method `bool run(list<string>)` (no `move`) compiles, then SEGVs at runtime

Filed 2026-08-09 by the review of `fix/progmeth` (the reserved-program-member guard). Found
while enumerating the accept set around the synthesized `bool run(move list<string>)` slot:
this spelling is NOT the reserved slot (the `move` qualifier makes it a different overload,
so the new guard correctly leaves it alone) and it is not a legal working program either.

**Severity: silent miscompile - compiles clean (rc 0), then the PROGRAM segfaults, rc 139,
zero output.** Pre-existing and out of scope for the guard commit: measured IDENTICAL on the
merge base binary and on `fix/progmeth`, so the guard neither caused nor changed it.

## Repro

`scratch/pm_mm_run_nomove.cb`, reproduced verbatim:

```cflat
import "list.cb";
program P {
    bool run(list<string> args) { return true; }
    int main(move list<string> args) { return 3; }
};
extern int main() {
    P p; list<string> a; list<string> b;
    bool u = p.run(b);
    p.run(a); p.WaitForExit();
    printf("u=%d ec=%d\n", u, p.exitCode);
    return 0;
}
```

Measured (macOS arm64 Release), merge base and branch alike:

| binary | compile rc | run rc | output |
|--------|-----------|--------|--------|
| merge base | 0 | 139 | (none) |
| `fix/progmeth` | 0 | 139 | (none) |

Contrast the neighbouring accept cells, which run correctly on both binaries: `int run()`
(different arity) prints its own value and leaves the synthesized run working, and
`bool run(move list<string>, int)` likewise. Only the `move`-less one-argument spelling
miscompiles.

## Root cause (hypothesis, not yet measured from IR)

The user's `bool run(list<string>)` and the synthesized `bool run(move list<string>)` are
distinct overload slots, but they differ ONLY in the `move` qualifier on the sole parameter.
The call `p.run(a)` at the driver is resolved to one of them while the argument is lowered
for the other - a borrowed `list<string>` passed where the callee expects to have taken
ownership (or the reverse), so the program thread starts against a list whose buffer has
already been zeroed or freed. `move` participates in mangling (`IsMove` on the parameter) but
the scorer's treatment of a move/non-move pair at the same arity is the thing to read first.

## Fix direction

Establish from `--no-opt` IR which overload each of the two `run` calls binds to and what the
argument's ownership state is at each. If the scorer genuinely cannot separate a move from a
non-move parameter at the same arity and type, that is the defect to fix. If it can, the
defect is in the argument lowering for the losing candidate. A hard error rejecting a user
`run(list<string>)` that shadows the synthesized move overload is the fallback only if the
resolution cannot be made correct - the reserved-member guard deliberately does NOT cover this
spelling, because it is a different slot.

A regression case belongs in `Test/test_program.cb` beside the `ReservedNameArity` /
`ReservedNameTimeout` legs, asserting the user `run`'s value AND `exitCode` in one program.
