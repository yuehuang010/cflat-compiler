# `move <call>().arr[i]` (element of a returned temporary) segfaults the compiler

Filed 2026-07-23 (found during the Stage 1 review of internal/plan/btree-borrowed-slot-recovery.md;
PRE-EXISTING - guard #2 requires a borrowed parameter, which a returned temporary never has, so
the shape never matched guard #2 before or after the IsElementAccess narrowing).

## Summary

Moving an ELEMENT of a function call's returned temporary crashes the compiler (rc=139,
segfault), e.g.:

```cflat
unique R* t = move mk().vals[0];   // mk() returns a node-like struct by value
```

The whole-field form `move mk().item` is cleanly rejected by the "requires an addressable
source" guard (MainListener.h ~14786). The element form slips past it because the subscript
produces a non-null Storage (a GEP into the materialized temporary), then the eager
null-store writes through a GEP into a temporary and the compiler crashes.

## Root cause (traced during review)

The addressability rejection at ParseMoveExpression ~14786 keys on null Storage; the
element-subscript path materializes the returned temporary and yields a real GEP, so the
guard does not fire and the downstream slot-move machinery (eager zero + recovery flags)
operates on a temporary that has no stable slot.

## Fix direction

Reject the element-of-temporary shape with the same addressability error the whole-field
form gets (detect that the subscript base is a materialized rvalue temporary, not a named
variable/param/global), via LogError - and per the CLAUDE.md rule, since this currently
manifests as a crash, the fix should turn it into a proper compile error. Add an err leg
(extend Test/errors/err_move_field_borrowed_param.cb or the closest addressability err
test) when fixing.
