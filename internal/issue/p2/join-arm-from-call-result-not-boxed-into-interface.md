# A join arm that is a CALL RESULT cannot be boxed into an interface

Filed 2026-08-05 while fixing [[chained-nullcoalesce-not-boxed-into-interface]]. Found by that
work's coverage matrix, not by its repro. It is NOT a chaining defect: it reproduces at chain
length 1, so the chain fix neither caused it nor closes it.

## Repro 1 - CALL ARGUMENT

```cflat
interface IShape { int area(); };
class Circle : IShape { int r = 0; int area() { return r * r; } };
int take(IShape s) { return s.area(); }
Circle* mk(int v) { if (v == 0) return nullptr; Circle* c = new Circle(); c->r = v; return c; }

extern int main() { return take(mk(0) ?? mk(3)); }
```

```
cc_calls1.cb(5,26): no overload of 'take' matches the given arguments.
```

## Repro 2 - DECLARATION INITIALIZER

```cflat
IShape j = mk(0) ?? mk(3);
```

```
cc_calls2.cb(4,30): cannot convert '??' arm to interface 'IShape': the arm's concrete class
cannot be determined; bind the arm to a local variable of the class type first
```

Both measured identical on `4c06cce` (pre-fix) and on the chain-fix binary - the chain fix changed
nothing here, in either spelling.

## Root cause

`ResolvePointerElementTypeName` answers a join arm from the DECLARED type of the binding a
`LoadInst` reads. A direct call result is not a load off a binding, so it resolves to empty and
both boxing sites bail. The bail is correct - nothing half-written, no IR emitted - the gap is
that a call's return type is a perfectly good source of the arm's class and is never consulted.

## Fix direction

Teach `ResolvePointerElementTypeName` to answer a `CallInst` from the callee's registered return
type (`functionTable`), the same way it answers a load from the binding's declared type. That is a
widening of a RESOLUTION helper, not of a rejection, so a miss degrades to today's bail. Audit the
other readers of that helper before widening: it also feeds `JoinArmsKeepOwner` and the `as`/`is`
paths, where a newly-resolvable arm changes an ownership verdict rather than only a boxing one.

Workaround: bind the call result to a local of the class type first -
`Circle* a = mk(3); take(z ?? a);` compiles and runs.

## Related

[[interface-issue-queue]]
