# Explicit generic-interface type argument in a base clause drops `*` and `unique`

Filed 2026-07-19.

## Summary

When a class names a generic interface in its base clause with an EXPLICIT pointer or `unique`
pointer type argument - `class PB : IB<R*>` - the type argument is stripped to the bare element
type. Both the mangled interface name and the substituted method signatures lose the `*` and the
`unique`, so the class can never satisfy the contract it just declared.

**Only the explicit form is affected.** The generic-parameter form `class Bx<T> : IB<T>` preserves
both qualifiers correctly. This distinction is the whole point of the issue - see Impact.

Independent of interface-scope `if const`; the repro below contains none.

## Repro

```cflat
class R { int id = default; };
interface IB<T> { void set(T value); };
class PB : IB<R*>
{
    R* _v = default;
    void set(R* value) { _v = value; }
};
extern int main() { PB p = default; return 0; }
```

```
mangle2.cb(2,27): class 'PB' does not implement 'IB__R::set'
```

Note `IB__R` - the `*` is gone from the mangled name, and the contract's `set` is now looking for a
by-value `R` parameter. Same failure for `IB<unique R*>`.

There is no workaround short of not using the explicit form.

## Root cause direction

`resolveImplsTypeArgs` (`cflat/MainListener.h`, in `ParseClassDefinition`, ~22142) builds the type
argument list with `entry->typeSpecifier()->getText()`, which does not carry the declarator suffix
(`*`) or the `unique` soft keyword from the `typeParameterEntry`. That string feeds both
`MangledGenericName` and the `activeTypeSubstitutions` map used by `InstantiateGenericInterface`, so
the loss shows up in the mangled name and in every method signature at once.

The generic-parameter path preserves the qualifiers somewhere this path does not. Diffing the two
type-argument resolution paths is the place to start. Whatever `PeelTypeArgSuffix` does for the
class path is the likely missing call.

## Impact

Narrow in practice. No core library uses the explicit form - `core/list.cb:8` is
`class list<T> : IEnumerable<T>, IList<T>, ICopyable<list<T>>`, the generic-parameter form.

**Recorded because it was once mis-scoped, and the mis-scoping was expensive.** An earlier write-up
of this bug claimed `is_unique(T)` / `is_pointer(T)` "can never be true inside a generic interface"
and named it the blocker for the `IList<T>` three-leg contract in
`internal/plan/unique-ownership.md`. That was measured on the explicit base-clause path and wrongly
generalized to both. It is false: verified 2026-07-19 with a three-leg interface whose `set()`
differs in move-ness per leg, dispatched virtually on a linked binary.

```
unique leg:  dtor 1 (on overwrite), tag=1, dtor 2 (scope exit)   <- container owns, frees once
borrow leg:  p alive=10, tag=2, dtor 10 (caller deletes)         <- container frees nothing
value leg:   tag=3
```

Discrimination is real, not permissive: a method declared only in the `is_unique(T)` branch is
correctly absent on the pointer leg (`unknown function 'uniqueOnly'`).

**Do not re-derive this as an `IList<T>` blocker.** It is not one.

## Related

- `internal/plan/unique-ownership.md` - D12 / the three-leg container contract, which this does
  NOT block.
- `internal/issue/if-const-no-constant-folding-path.md` - separate gap; conditions must be spelled
  as chained `else if const` rather than `&&`.
