# The BOXED join proof never retires a rebound arm, so a sole-owner box is false-rejected

Filed 2026-08-07 by `fix/join-nullarm`, which measured it while building that fix's accept set.
PRE-EXISTING for every arm spelling except the null-valued LOCAL one, which `fix/join-nullarm`
brought into line with the rest.

## Root cause

The RAW pointer-join proof records every arm's SLOT and re-asks it at each consumer
(`JoinArmsStillKeepOwner`), so nulling or rebinding an arm after the join retires the fact and the
receiver's `delete` is accepted - which is correct, because the receiver then holds the only
reference and a rejection would LEAK.

The BOXED path has no such re-ask. `BoxInterfaceJoinArms` writes `SourceKeepsOwner` into the
per-arm `InterfaceBoxRecord` at the boxing site, `TagInterfaceBoxProvenance` folds it into a single
bool on the interface local at the DECLARATION, and the `delete` guard reads that bool. Nothing
between the declaration and the delete can retire it.

## Repro

Common prelude:

```cflat
int dtorCount = 0;
interface IS { int area(); };
class Ci : IS { int r = 7; int area() { return r; } ~Ci() { dtorCount = dtorCount + 1; } };
```

Rejected, on `19d8727` and after `fix/join-nullarm` - every spelling of the arm:

```cflat
Ci* c = new Ci();
IS s = nullptr ?? c;   // also `c ?? c`, also `idb(true) ? c : nullptr`
c = nullptr;           // `c` no longer frees anything; `s` is the sole owner
delete s;              // REJECTED: "it boxes an object that 'c' already frees"
```

The RAW twin of the same program is ACCEPTED and runs at one free on both binaries:

```cflat
Ci* c = new Ci();
Ci* b = nullptr ?? c;
c = nullptr;
delete b;              // accepted, exit 0, dtorCount 1
```

The remedy the diagnostic names ("let 'c' release it") does not exist - `c` is null - so the
program has to leak to satisfy the compiler. That is the false-rejection polarity this family
treats as the expensive direction.

## What `fix/join-nullarm` changed here

Only the population. A join arm that is a LOCAL provably parked at null used to make the whole
boxed proof collapse (its own bug, closed by that commit); now it is neutral, so
`IS s = n ?? c; c = nullptr; delete s;` joins the literal-arm spellings above in being rejected.
That is PARITY with the pre-existing behaviour of every other arm spelling, not a new class of
rejection - and the raw/boxed disagreement it exposes is pre-existing too (measured: raw literal
accepted, boxed literal rejected, both on `19d8727`).

## Fix direction

Give the boxed proof the arm-side re-ask the raw proof already has: record the arms' SLOTS
alongside the interface local's borrowed flag and re-ask them at the `delete` guard, the same
`!PointerRebound` test `JoinArmsStillKeepOwner` uses. The accept set is the `join_arm_nulled_*` /
`join_arm_rebound_*` / `join_nullarm_retired_*` legs in `Test/test_move.cb` plus their boxed twins,
and the reject set must stay exactly the legs in
`Test/errors/err_delete_borrowed_interface_box.cb`.

P3: a working remedy exists (do not null the arm, or box a `new`), and the direction is a leak the
programmer can see rather than a silent double free.
