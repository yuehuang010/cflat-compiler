# `o = *op` where `op` points AT `o` drops the value instead of being a no-op

Filed 2026-08-09 by `fix/owncopy`. Degenerate self-assign spelled through a pointer.

```cflat
Box o = makeBox(5);   // Box { unique Res* item; }
Box* op = &o;
o = *op;              // BEFORE: rc 133, printed a use-after-free id (garbage)
                      // AFTER : rc 0, `o.item == nullptr` - the value is silently LOST
```

Measured pre/post on `fix/owncopy`: `PRE compile 0, rc 133, mid id=4 dtor=1` ->
`POST compile 0, rc 0, mid id=-1 dtor=1, end dtor=1`. The double free is gone (memory-safe, one
free for one allocation) but the destination ends up null.

## Root cause

The owning-value reassign arm guards self-assign with pointer identity
(`destination != rightNV.Storage`). For a deref source, `Storage` is the LOADED pointer value, not
the destination alloca, so the guard cannot see that the two name the same object: the arm
destructs the old destination, stores the (already-freed) bits back, and then zeroes the source -
which IS the destination. The same blind spot is documented on the deref-DESTINATION arm ("use
after move through a pointer is left unenforced - a deref source has no name").

## Fix direction

Needs a provable same-object test for a deref lvalue, not just SSA pointer identity (the existing
`ProvablyDifferentSlots` / `AddressRootIsStackOrGlobal` helpers are the starting point), or a
runtime address compare like the one `EmitUniqueFieldDelete` already uses for a self-assigning
`unique` field. Do NOT simply suppress the source-zero: the old destination has already been
destructed by then, which is the pre-fix double free.
