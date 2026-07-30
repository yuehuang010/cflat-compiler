# Constructor discriminator is inconsistent: name-only outside a lock/program body, but null-declarationSpecifiers inside one

Found 2026-07-27 during review of `fix/iface-segv` (round 3). The fix landed in that
branch established the CORRECT discriminator for "is this a constructor": per the grammar
(`CFlat.g4:783`, `829`, `833`), a constructor is a `functionDefinition` with NO
`declarationSpecifiers` - name matching alone is not sufficient, since an ordinary method
can share its class's or program's name and still have a return type (e.g. `int C()`).

That corrected discriminator (`func->declarationSpecifiers() == nullptr && getFunctionName(func) == <name>`)
was applied ONLY at the five call sites the branch touched (constructor-in-lock-group and
constructor-in-'program' rejection, scanner and codegen). Six PRE-EXISTING sites that also
decide "is this func the constructor" still use the OLD, name-only rule:

- `cflat/MainListener.h:1793` (ForwardRefScanner member loop - constructor-overload
  pre-declaration: `getFunctionName(func) == baseTypeName`)
- `cflat/MainListener.h:22626` (`funcName == baseName`)
- `cflat/MainListener.h:22940` (`getFunctionName(f) == baseName && !f->parameterTypeList()`)
- `cflat/MainListener.h:23119` (`funcName == baseName`)
- `cflat/MainListener.h:25343` (`getFunctionName(f) == baseName && !f->parameterTypeList()`)
- `cflat/MainListener.h:25534` (`funcName == baseName`)

(Line numbers as of `fix/iface-segv` @ commit after round 3; re-grep for
`getFunctionName(f) == baseName` / `funcName == baseName` / `typeName` style comparisons
if they have since shifted.)

## Repro - identical on master AND on `fix/iface-segv`, so NOT a regression

```cflat
class C { int v = 0; int C() { return 7; } };
extern int main() { C c; return c.C(); }
```

```
$ cflat repro.cb -i Test/library --check
repro.cb(2,32): no overload of 'C' matches the given arguments.
  Call arguments (1):
    [0] C <unnamed>
  Candidates (1):
    _C_C__()
```

`int C()` - a method with a return type, hence NOT a constructor by the grammar rule - is
silently registered and mangled as the class's CONSTRUCTOR overload
(`ScanStructOrClassDefinition`'s `getFunctionName(func) == typeName` branch at line 1793
takes the "constructor overload" path instead of calling `ScanFunctionDefinition`), so it
is never callable as `c.C()`.

Contrast: the SAME construct, `int C()`, written inside a `lock (...) { }` field group
(fixed in `fix/iface-segv`, see `Test/test_sync.cb`'s `LockAccount.LockAccount()`) IS
correctly treated as an ordinary method and IS callable. The codebase now has two
contradictory rules for the same question, depending on whether the method happens to sit
inside a lock field group / `program` body or not.

## Root cause

The six sites above use `getFunctionName(func) == baseName` (or `== typeName` /
`== structName`, all name-only) as the constructor test, instead of
`func->declarationSpecifiers() == nullptr && getFunctionName(func) == baseName` (the
corrected discriminator introduced in `fix/iface-segv`).

## Fix direction

Apply the same `declarationSpecifiers() == nullptr` test at all six sites, so a same-named
method with a return type is registered and dispatched as an ordinary method everywhere,
not just inside a lock group / `program` body.

**BEFORE touching this for consistency: sweep `core/*.cb` and `example/` (and `Test/*.cb`)
for any class or struct with a member function that shares its own type's name (the
name-only rule's exploitable case).** The round-2 mistake on `fix/iface-segv` was tightening
a constructor guard without first proving no working code relied on the looser (buggy)
behavior - that turned working code into a hard compile error. The same risk applies here
in reverse: something might currently rely on `int C()` being silently treated as a
constructor-only declaration (unlikely, since it is uncallable as an ordinary method today,
but confirm before changing behavior for anyone already declaring - and never calling -
such a member).

## Explicit warning for whoever picks this up

Before "fixing" this for consistency, sweep `core/*.cb` and `example/` (and `Test/*.cb`)
first, as stated in the fix direction above. Do not skip that step - the round-2 mistake on
`fix/iface-segv` was tightening a constructor guard without first proving no working code
relied on the looser (buggy) behavior, which turned working code into a hard compile
error. Prove the same cannot happen here before changing any of the six sites.

## Impact

Low severity: the failure mode is a clear "no overload matches" diagnostic, not a crash or
miscompile, and requires a user to intentionally name a method identically to its own
type - an unusual pattern. Filed for correctness/consistency, not urgency.
