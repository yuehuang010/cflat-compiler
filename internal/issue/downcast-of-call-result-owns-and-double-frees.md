# `expr as T` on a function-call result yields an OWNING pointer (double-free / UAF)

## Summary

When the source of an `as` downcast is a *function call result* (rather than a named
variable), the resulting pointer is tracked as **owning**: the compiler auto-`delete`s
it at scope exit. If the real owner also frees the object, this is a double-free; if the
downcast lives inside a closure, the object is freed on every closure return.

Downcasting a **named local** (even one bound from the same call) is correctly treated
as a borrow.

## Repro

```cflat
interface Animal { int sound(); };
class Dog : Animal { int x = 0; int sound() { return 1; } };
list<Animal> _zoo = default;
alias Animal activeAnimal() { return _zoo.get(0); }

extern int main() {
    _zoo.add(new Dog());
    Dog* d = activeAnimal() as Dog;   // BUG: `d` is treated as owning
    d.x = 7;
    Animal owned = _zoo.take(0); delete owned;   // the real owner frees it
    return 0;   // scope exit auto-deletes `d` again -> double-free (ASan aborts)
}
```

ASan: `attempting double-free`. Build with `--asan` to observe; `--heap-audit` does not
flag it (double-free detection was removed), so it manifests only as an intermittent
heap-corruption crash at exit.

Closure variant (more dangerous - frees a live object each call):

```cflat
_cb = () => { Dog* d = activeAnimal() as Dog; d.x = d.x + 1; };
_cb();   // ok
_cb();   // use-after-free: the Dog was freed at the first closure's return
```

## Workaround (in use)

Bind the call result to a named local first, then downcast the local:

```cflat
Animal a = activeAnimal();
Dog* d = a as Dog;     // borrow - not auto-deleted
```

`example/ui/fedit/fedit.cb` (wireHandlers + feditSelfTest) and the P7 UI hosts use this
two-step form deliberately; see the comments there.

## Root cause (hypothesis, unverified)

The ownership/borrow classifier likely treats an `as`-cast of an r-value (call result)
as producing a fresh owned value, whereas an `as`-cast of an l-value (named var) yields
a borrow of the existing storage. The fix is to make `as` of a borrow-typed r-value
(e.g. an `alias`-returning call) also a borrow.

## Fix direction

In the `as`/downcast codegen + move-analysis, propagate the borrow/alias-ness of the
operand: `borrow as T` -> borrow; only `owned-temporary as T` -> owned. Add a regression
test mirroring the repro above once fixed.
