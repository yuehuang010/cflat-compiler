# A mixed owned/borrowed view in a field is sunk through a pointer parameter and double-frees

Pre-existing on master; found by the T387 Sol review round 3 (2026-10-21). T387 refuses the same join when the sink names the
field directly (`sink(move a.hs[0].f)`), but not through a pointer to the aggregate.

## Repro

`scratch/repro_keep/box1021/t387_rev3/alias_helper.cb` (also `alias_local_pointer.cb`; the owned counterpart `alias_owned.cb` is clean):

```cflat
void use(HA* p) { sink(move p->hs[0].f); }
int main(int argc, char** argv) { bool c = argc > 1; S[] w = new S[2]; HA a; a.hs[0].f = c ? new S[1] : w; use(&a); return 0; }
```

Master and T387: compile rc 0, ASan 134 at argc 1 (borrowed `w` moved into the sink, freed twice), clean at argc 2.

## Root cause

T387's non-local view state is keyed by the resolved root binding inside one function. A sink reached through a pointer (a helper's
`HA* p`, or a local `HA* q = &a`) has no key for the caller's slot, so the may-borrowed state is not consulted.

## Fix direction

Local pointer alias (`q = &a`): resolve the pointer's provenance to the local root when it is proven (same family as the T382 pointer
receiver proof). Helper parameter: a per-parameter "sinks a view field" callee fact checked at the call site when the argument's slot is
may-borrowed - or refuse passing the address of an aggregate whose view field is may-borrowed to a function that moves out of it.
