# A `?:` mixing a `move` of an interface-typed local with a thin arm fails to compile

Filed 2026-07-26 while writing regression coverage for
`internal/plan/unique-interface-move-readable-null.md` (Part 2, move-arm ledgering). Discovered
via a scratch repro, not exercised by any landed test - both `test_move.cb` and
`err_move.cb` avoid this shape.

## Repro

```cflat
interface IShapeMove { int area(); };
class SqMove : IShapeMove { int s = 3; int area() { return s * s; } };

extern int main()
{
    unique IShapeMove g = new SqMove();
    bool c = true;
    unique IShapeMove k = c ? move g : new SqMove();   // <- here
    return k.area();
}
```

Observed: `Error: ternary branches have incompatible types '__iface_fat_ptr' and 'pointer'`
(`cflat/MainListener.h:10956`). Same error (arm order and type names swapped) if the arms are
reversed, and the same error if the thin arm is `nullptr` instead of `new SqMove()`:

```cflat
unique IShapeMove k = c ? move g : nullptr;   // same "incompatible types" error
```

By contrast, `move` of a NON-interface-typed local mixed with `new` arms works fine (both arms
already-fat via `UpcastTernaryPhiToInterface`, e.g. `identityBool(false) ? new SqMove() : new
CiMove()`, `test_move.cb:1906`), and `move` on BOTH arms of an already-fat-typed pair also
compiles and runs correctly:

```cflat
unique IShapeMove g = new SqMove();
unique IShapeMove g2 = new SqMove();
unique IShapeMove k = c ? move g : move g2;   // compiles, correct dtor accounting
```

## Root cause

`move` of an interface-TYPED local (`g`'s static type is `IShapeMove`) evaluates directly to the
fat `{i8*,i8*}` struct type, since the local itself is already fat-typed - there is no lazy boxing
step for it. A `new SqMove()` arm (or `nullptr`) is still thin/untyped at the point the generic
ternary type-harmonizer (`cflat/MainListener.h:10900-10960`, `HarmonizeTernaryArmTypes` or
equivalent) runs; that harmonizer only knows how to reconcile int/int, float/float, int/float,
and string/pointer-literal pairs, plus an exact type match. It has no case for "one arm is
already the interface fat-pointer struct, the other is a thin pointer that would normally get
boxed via `UpcastTernaryPhiToInterface` at the declaration-init site" - so it falls through to the
`LogErrorContext` "incompatible types" branch before that later boxing pass ever runs.

## Fix direction

Add a harmonizer case: when one arm's type is the fat interface struct (`GetFatPtrType()` /
`__iface_fat_ptr`) and the other is a thin pointer, box the thin arm into the interface fat
struct in its own block (mirroring what `UpcastTernaryPhiToInterface` already does for uniformly-
thin arms), rather than rejecting the pair outright. `nullptr` needs the same treatment (thin
null constant boxed to a zeroed fat struct) since the failure reproduces identically with a
`nullptr` arm.

## Test fallout

Two coverage items from the move-of-interface plan are blocked by this gap and were deliberately
NOT added as passing tests: `unique IShapeMove k = c ? move g : new SqMove();` and `unique
IShapeMove k = c ? move g : nullptr;` (both directions of `c`). Once fixed, add these as runtime
legs near `testUniqueInterfaceReassign`/`testUniqueInterfaceTernary` in `Test/test_move.cb`,
predicting: `move`-arm taken transfers ownership (source nulled, one free at scope exit); the
other arm taken leaves the move source's object intact and the `new`/`nullptr` arm never runs
(short-circuit), so `g`'s own destructor fires at its own scope exit.
