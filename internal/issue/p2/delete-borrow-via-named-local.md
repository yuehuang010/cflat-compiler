# Borrowing container through a helper with an explicit destination argument

## Remaining scope after T507

T507 closes the reported `Bag.put -> put2 -> items.add` path when the container destination is a
method receiver, and rejects a proven `function<T>` local target that stores into a global
borrowing container. The remaining conservative gap is forwarding through helpers when the sink
destination is an explicit pointer parameter whose lifetime cannot be mapped through the chain.

## Repro

The existing accepted leg is `Test/test_list_ownership.cb`, `helperDepth1` -> `helperDepth2`:

```cflat
void helperDepth2(list<Res*>* l, Res* p) { l->add(p); }
void helperDepth1(list<Res*>* l, Res* p) { helperDepth2(l, p); }

Res* p = new Res(27);
list<Res*> l;
helperDepth1(&l, p);
delete p;
```

Master accepts this leg. The list is caller-local, but the current frozen T507 accept-set requires
it to remain accepted.

## Root cause

The interprocedural summary forwards globals and method-receiver destinations. A direct explicit
container pointer is recorded as an unmapped outliving destination, so a direct helper call can
be diagnosed, but that destination is not propagated through another helper. Propagating it
would reject the frozen `two-level helper stores` leg above. Resolving the distinction would
require a policy change to that accepted behavior or stronger lifetime information.

## Fix direction

Keep unknown explicit destinations accepting until the accepted behavior is changed deliberately.
If the policy changes, add a caller-visible destination provenance summary and update the exact
`two-level helper stores` expectation only with explicit authorization.
