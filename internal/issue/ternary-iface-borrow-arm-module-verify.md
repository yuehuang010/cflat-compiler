# A '?:' bound to an interface slot with a BORROWED thin-pointer arm fails module verification

Filed 2026-07-26 while implementing Part 2 of
`internal/plan/unique-interface-move-readable-null.md`. Pre-existing on `master`; unrelated to
the mixed fat-pointer join fix landed with it (that fix handles the ALREADY-FAT join shape).

## Repro

```cflat
interface IShapeMove { int area(); };
class SqMove : IShapeMove { int s = 3; SqMove() { } int area() { return s * s; } };
bool identityBool(bool b) { return b; }

extern int main()
{
    unique SqMove* owner = new SqMove();
    IShapeMove k = identityBool(false) ? new SqMove() : owner;   // <- here
    return k.area();
}
```

Observed: `Error: module verification failed.` with no source location. Expected: either a
working boxed join, or a LogError naming the arm that cannot be boxed.

Note the `unique IShapeMove k = ...` spelling of the same expression errors cleanly
("cannot initialize unique 'k' from a borrowed value"), because the D5 reject fires before the
module is verified. Only the PLAIN-receiver spelling reaches the verifier.

## Root cause

`UpcastTernaryPhiToInterface` (`cflat/MainListener.h`) resolves each arm's concrete class from
`FindValueElementTypeName`, whose ledger (`valueElementTypeNames_`) is populated only at `new`
sites. A BORROWED arm is a plain load of a local, so it is in no ledger; the function bails with
`return nullptr` and the caller falls through to bitcasting a raw `ptr` into the `{i8*,i8*}` fat
struct - invalid IR that only the module verifier catches.

## Fix direction

Either widen the arm-class resolution to a named source (recover the class from the arm's
NamedVariable / declared type rather than from the `new` ledger), or - if that arm shape is to
stay unsupported - detect the failed upcast at the two call sites (`cflat/MainListener.h`, the
declaration-init and `=` paths) and emit a LogErrorContext naming the unboxable arm instead of
emitting the bitcast.
