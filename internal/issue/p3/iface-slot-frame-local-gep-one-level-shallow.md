# `InterfaceSlotIsFrameLocal` is one GEP level too shallow for field/element receivers

Carried over from the now-deleted null-interface-access-widening plan (all three
widening stages landed). This is the one structural gap that plan found by reading rather
than by probing, and it was never closed.

## Summary

`InterfaceSlotIsFrameLocal` (`cflat/LLVMBackend.h`, around line 19619) walks the alloca's
users exactly one GEP level deep: a constant GEP is allowed only if *its* users are loads
or stores into it. That is deep enough for a plain local:

```
%lv -> GEP(fatTy, %lv, 0) -> load
```

and one level too shallow for a struct field or an array element in the METHOD form:

```
%h -> GEP(%h, 0, 0)  [field c] -> GEP(fatTy, ., 0)  [vtable field] -> load
```

The inner GEP is itself a GEP user, so the walk returns `false` and the null-interface
dispatch is silently accepted instead of being proven and diagnosed.

## Impact

Not a soundness hole in the emitted code - the accepted case still faults at run time the
way it always did. The cost is a missing compile-time diagnostic: a provably-null method
call through a field or array-element receiver is not caught, while the same call through a
local is. `doc/LANGUAGE.md` (Interface Fields) documents the current split honestly - it
says nested field/element paths fall back to a runtime fault - so closing this gap would
tighten the docs, not contradict them.

## Fix direction

Let the users-walk recurse through chained constant GEPs instead of stopping at depth 1,
keeping the existing requirement that the terminal users are loads or stores into the slot.
Guard against unbounded recursion and against non-constant indices.

Before widening, re-read section 2b of the old plan's accept set: some receivers are
DELIBERATELY accepted and must stay accepted. The plan also recorded that the guard's
narrowness rests on an argument, not on test coverage - a build that dropped all four
anchor conditions still passed the whole suite - so a change here will not be caught by
the existing tests. Add a regression case to an existing `Test/errors/err_iface_*.cb`
covering the field-receiver method form.
