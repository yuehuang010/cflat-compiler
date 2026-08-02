# `delete` of a plain copy or `alias` local of an owning pointer is not diagnosed (double free)

Filed 2026-08-02, measured while closing the borrowed-interface-box delete
([[interface-boxing-keyed-on-source-binding]]). Not boxing-specific - the raw-pointer spelling has
the same hole, so this is a gap in the borrow tracking itself, not in any one guard.

## Repro

Both of these compile clean and abort at runtime (exit 134) on `master` = `ca5a02a`, and still do:

```cflat
int dtorCount = 0;
class Ci { int r = 0; ~Ci() { dtorCount = dtorCount + 1; } };

extern int main()
{
    Ci* c = new Ci();
    Ci* b = c;          // or: alias Ci* b = c;
    delete b;           // `c` is IsOwning and frees again at scope exit
    return 0;
}
```

The interface-boxed spelling reaches the same double free through a box:

```cflat
Ci* c = new Ci();
Ci* b = c;              // or: alias Ci* b = c;
IS s = b;
delete s;
```

## Root cause

`Ci* b = c;` and `alias Ci* b = c;` both produce a local that is neither `IsOwning` nor flagged as
a borrow: `IsBorrowed` is propagated from the RHS, and an OWNING RHS is not borrowed, so nothing is
set. `IsAliasBorrow` is for a local bound from an `alias` RETURN, not for the `alias` declaration
spelling. So at `delete b` - and at any boxing site fed from `b` - nothing distinguishes `b` from a
local that legitimately owns the object.

## Why it was not closed with the interface-box work

The interface-box guard rejects only on a POSITIVE proof that another owner frees the object, taken
from the source binding. `b` carries no such proof. Inferring one from the absence of `IsOwning`
is the exact polarity error that round of work had to back out: a pointer that received its `new`
in a LATER statement (`Ci* c = nullptr; c = new Ci();`) is ALSO not `IsOwning`, and it IS the sole
owner, so rejecting on that negative false-rejects correct code.

## Fix direction

Establish the borrow at the DECLARATION, where both sides are in hand: when the initializer of a
pointer local is a plain read of a live `IsOwning` binding (and no `move` / no ownership transfer
ran), mark the new local as a borrow of that binding - the flag the existing `IsBorrowed` /
`BorrowedOrigin` pair already carries for the parameter-alias case, which the delete guards and the
boxing proof both already consult. The `alias T* b = c;` declaration spelling should set the same
flag unconditionally, since `alias` is the user saying "borrow" out loud.

Both existing consumers then close at once: the raw `delete b;` guard at the `IsBorrowed` check in
`ParseDeleteExpression`, and `BindingKeepsOwnershipOfBoxedObject` in `MainListener.h`.

## Related

[[interface-boxing-keyed-on-source-binding]] [[interface-issue-queue]]
