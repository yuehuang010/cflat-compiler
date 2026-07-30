# An IsAliasBorrow owning-struct LOCAL launders its borrow through `=` and through `move`

Filed 2026-07-26 during review of the mixed-'?:'-owning-struct-join fix
(commit "Stop a mixed '?:' owning-value struct join from double-freeing its borrowed arm").
PRE-EXISTING and independent of that commit: the same two holes reproduce on master with a
GENUINE `alias`-return local, which has been the `IsAliasBorrow` shape all along. The join fix
merely adds a second way to create such a local, so it inherits the holes rather than causing them.

## Repro

All four use the same shapes:

```cflat
int dtorCount = 0;
struct Res { int id = 0; ~Res() { dtorCount = dtorCount + 1; } };
struct Box { unique Res* item = nullptr; };
struct Wrap { Box b; alias Box get() { return this.b; } };
Box makeBox(int id) { Box x; x.item = new Res(); x.item->id = id; return x; }
void sink(move Box b) { }
bool ident(bool b) { return b; }
```

1. `scratch/r11.cb` - MASTER shape, assignment. An `alias`-return local assigned into an owning local:

```cflat
Wrap w; w.b = makeBox(5);
Box k = w.get();        // k is IsAliasBorrow: dtor suppressed, correct
Box other = makeBox(1);
other = k;              // other ADOPTS the borrow -> double free with w.b
```

2. `scratch/r12.cb` - MASTER shape, move argument: `Box k = w.get(); sink(move k);` - the `move`
   zeroes `k` (already harmless) and the `move` parameter's scope-exit teardown frees `w.b`'s
   pointee, which `w` frees again.

3. `scratch/r10.cb` - the same assignment hole reached through a mixed '?:' join local:
   `Box k = c ? makeBox(7) : borrowed; Box other = makeBox(1); other = k;`

4. `scratch/r9.cb` - the same move-argument hole reached through a mixed '?:' join local:
   `Box k = c ? makeBox(7) : borrowed; sink(move k);`

## Observed

All four abort (exit 134) at the real owner's scope exit, after the laundered receiver has already
run the pointee's destructor once. r11/r12 abort identically on master.

## Root cause

`NamedVariable::IsAliasBorrow` (`cflat/LLVMBackend.h`) means "this local shallow-aliases storage it
does not own", and it correctly suppresses the local's own scope-exit destructor
(`EmitDestructorsForScope` / `DropValue` consult it). But only THREE persist sites consult it:

- `RejectAliasStoreIntoField` (`cflat/MainListener.h`) - the `=` field store,
- the same reject from `EmitOneFieldInit` - the brace-init field store,
- the `return`-site reject ("cannot return an 'alias' value ...").

Two persist sites do NOT:

- **Assignment into an owning LOCAL** (`other = k`). The owning-value reassignment block in
  `ParseAssignmentExpression` gates on `rightNV.Storage` being an alloca/global with a non-empty
  `CallerName` - which an `IsAliasBorrow` local satisfies - and never consults `IsAliasBorrow` or
  `TypeAndValue.IsAlias`. `ClassifyOwningAssignSource` then classifies the non-copyable owner as a
  MOVE, so the borrow's bits are transferred into a destination that DOES destruct.
- **A `move` ARGUMENT bound to a `move` parameter** (`sink(move k)`). `ParseMoveExpression`'s
  by-value struct branch and `ApplyMoveParamTransfer` likewise never consult `IsAliasBorrow`, so
  the callee's `move` parameter adopts and destroys a value the real owner still holds.

The field/return sites got the check because those were the reported crashes; the local-assignment
and move-argument sites were simply never covered.

## Fix direction

Route all five persist sites through ONE predicate ("is this source a borrow of storage it does not
own": `TypeAndValue.IsAlias || IsAliasBorrow`) instead of three ad-hoc checks, and reject at the two
uncovered sites with the existing `alias`-store wording. The `move`-argument site should reject at
the argument, not in the callee, so the diagnostic can name the borrowed local.

Watch out for the legitimate chained-borrow spelling `Box k2 = k;` at DECLARATION, which must keep
working - a declaration propagates `IsAliasBorrow` to the new local (it does not adopt), and is the
recommended way to pass a borrow along.

## Related

- The '?:' join fix that surfaced these (`Test/test_move.cb` `testOwningStructTernaryJoin`,
  `Test/errors/err_move.cb` `JoinBox` legs) - it marks a mixed-join receiver `IsAliasBorrow`, which
  is sound at the declaration itself and only leaks; these two sites turn that leak into a
  double free.
