# Compiler SIGSEGVs on a generic `unique` field assigned from a TEMP whose pointee has a destructor

Filed 2026-08-01 by the round-1 adversarial review of `fix/unique-f2f`. **Pre-existing** -
verified identical on master `3b6e3e8` and on that fix branch, so it is neither caused nor
worsened by that change.

Severity: **COMPILER CRASH, exit 139, ZERO output.** No diagnostic, no partial error text, no
LLVM message - the process dies silently. Per CLAUDE.md's convention this must become a proper
compiler error once the root cause is known.

## Repro

```cflat
class Item { int v = default; ~Item() {} };
struct Box<T> { T t = default; };
Box<unique Item*> makeBox() { Box<unique Item*> b = default; b.t = new Item(); b.t->v = 70; return b; }
extern int main() { Box<unique Item*> c = default; c.t = makeBox().t; printf("temp %d\n", c.t->v); return 0; }
```

`cflat scratch/cls_temp.cb -o cls_temp` -> exit 139, no output at all.

## The trigger needs ALL THREE conditions - drop any one and it compiles

Measured on `3b6e3e8`:

| Variant | Compiler |
|---|---|
| the repro above (user dtor + generic field + temp source) | **exit 139, no output** |
| `class Item` with NO user destructor, temp source | exit 0, compiles and links |
| user destructor, but source is a NAMED LOCAL (`c.t = a.t`) | exit 0, compiles and links |
| `struct Item` (no destructor at all), temp source | exit 0, compiles and links |
| user destructor + temp source, but a PLAIN struct field (`Holder.slot`) | exit 1, correctly diagnosed |

The last row is the useful one: the plain-field spelling of the very same program reaches the
existing field-to-field diagnostic and reports cleanly. Only the GENERIC field
(`Box<unique Item*>::t`) with a CALL-RESULT source and a destructor-bearing pointee crashes.

## Relationship to the field-to-field work

This shares the temp-source spelling with
[[unique-field-to-field-residue-temp-and-interface-source]] but is a DIFFERENT failure: that one
is a missing diagnostic (program compiles, then double-frees at exit 134); this one kills the
compiler before it emits anything. The distinguishing input is the user-written destructor -
add `~Item() {}` to that issue's temp-source repro and the runtime double free becomes a
compile-time SIGSEGV. Do not assume one fix closes both, but probe them together: the crash
plausibly lives in the same destructor-synthesis / temp-materialization path the missing
diagnostic fails to reach.

## Root cause direction - UNDIAGNOSED

Not investigated. Start by running the repro under a debugger or with `-v` to get past the
silent death; `CompilerManager.h` installs crash handlers that should dump compiler state, so
find out why nothing was printed here - a crash handler that produces no output on a real
SIGSEGV is itself worth a look.

## Test coverage

None. Wants an `expect_error` leg once the crash becomes a diagnostic; until then it cannot be
expressed in the suite at all.

Related: [[unique-field-to-field-residue-temp-and-interface-source]], [[interface-issue-queue]]
