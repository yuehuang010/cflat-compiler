# A CHAINED `??` (`a ?? b ?? c`) into an interface is still not boxed

Filed 2026-07-31 as the named residue of `fix/iface-join-return-boxing`, which closed the single
`??` join into an interface in RETURN and CALL-ARGUMENT position. A chain of two or more `??`
does not work in either position. Found by review, not by the original repro - the campaign's
recurring "half done along a spelling axis" pattern (see `internal/fix-issue-lessons.md`).

## Repro 1 - CALL ARGUMENT (unchanged from master)

```cflat
interface IShape { int area(); };
class Circle : IShape { int r = 0; int area() { return r * r; } };
int take(IShape s) { return s.area(); }

extern int main()
{
    Circle* a = new Circle(); a->r = 3;
    Circle* y = nullptr;
    Circle* z = nullptr;
    return take(z ?? y ?? a);
}
```

```
chain.cb(10,17): no overload of 'take' matches the given arguments.
```

Identical on master and on the branch.

## Repro 2 - DECLARATION INITIALIZER (also broken, and PRE-EXISTING)

```cflat
IShape j = z ?? y ?? a;
```

```
chain.cb(9,15): cannot convert '??' arm to interface 'IShape': the arm's concrete class
cannot be determined; bind the arm to a local variable of the class type first
```

Worth stating plainly: the decl-init boxing site landed in `d1935a2` and this spelling never
worked there either, so the chained gap is NOT a residue of the return/argument work - it spans
every position that boxes a `??`. The assignment spelling behaves the same way.

## Repro 3 - RETURN (improved, still does not compile)

```cflat
IShape pick(Circle* p, Circle* q, Circle* r) { return p ?? q ?? r; }
```

```
chainret.cb(3,47): cannot convert '??' arm to interface 'IShape': the arm's concrete class
cannot be determined; bind the arm to a local variable of the class type first
```

Master emitted a bare `Function return type does not match operand type of return inst!` verifier
dump with no source location here, so this spelling did improve - it is now a LOCATED rejection
rather than a crash. It still does not compile.

## Root cause

`a ?? b ?? c` lowers as `a ?? (b ?? c)`, and each `??` joins through its own SLOT. The OUTER
join's right arm is therefore the inner join's result: a `LoadInst` off the inner slot, not a
pointer to an object. `ResolvePointerElementTypeName` cannot name a class for it, so
`BoxInterfaceJoinArms` reports an unresolvable arm (return position) and
`BoxNullCoalesceJoinArgument` bails (argument position, leaving the ordinary located diagnostic).

Both behaviours are correct for a bail - nothing half-written, no IR emitted, no wrong function
selected. The gap is that nothing FLATTENS the chain.

## Fix direction

Flatten at the ledger, not at the boxing site. `RegisterNullCoalesceJoin` already records the
arms of each `??`; when an arm's value is itself a ledgered join, splice that join's arms into
the outer entry instead of recording the load. The arms of `a ?? b ?? c` then become
`{a, b, c}`, each in its own block, and both existing consumers work unchanged - per-arm boxing
already handles N arms and mixed classes.

Watch the block bookkeeping: each spliced arm must keep ITS OWN predecessor block (the one whose
terminator the arm's box is emitted before), not the outer join's, or the fat phi gets an
incoming edge from a block that does not branch to it.

Workaround today: bind the chain to a PLAIN POINTER local first, then use that -
`Circle* pick = z ?? y ?? a; take(pick);` compiles and prints 9. Binding it straight to an
INTERFACE local does NOT work (that is Repro 2); the intermediate must be the concrete `T*`.

## Related

[[interface-issue-queue]]
