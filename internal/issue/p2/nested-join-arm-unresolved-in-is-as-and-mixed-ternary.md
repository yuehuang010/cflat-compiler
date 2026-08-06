# A NESTED join arm is still unresolved at the `is`/`as` and mixed-ternary sites

Filed 2026-08-05 as the named, measured residue of the chained-join boxing fix
([[interface-issue-queue]], "Chained and nested joins into an interface"). That fix taught the two
BOXING sites to recurse into an arm that is itself a join. Two OTHER sites ask the same question -
"what concrete class is this arm?" - and were deliberately left alone. Both are measured identical
on `4c06cce` and on the fix binary, so neither is a regression.

## Site 1 - `ResolveTernaryArmClasses` in `cflat/MainListener.h`, the `is`/`as`-against-a-CONCRETE-class path

```cflat
interface IShape { int area(); };
class Circle : IShape { int r = 0; int area() { return r * r; } };
bool ib(bool b) { return b; }

extern int main()
{
    Circle* a = new Circle(); a->r = 3;
    Circle* b = new Circle(); b->r = 5;
    return (ib(true) ? (ib(false) ? a : b) : a) is Circle ? 9 : 0;
}
```

```
s_isconcrete_ternchain.cb(5,9): cannot test '?:' arm against 'Circle': the arm's concrete class
cannot be determined; bind the arm to a local variable of the class type first
```

Identical on both binaries. Note the CONTRAST that makes this a real residue and not a duplicate:
the same chain through `as <Interface>` now WORKS (measured 25 post-fix, "cannot convert '?:' arm"
pre-fix), because that spelling routes through the boxing path that was fixed. Only the
concrete-class predicate is left.

Why it was not folded in: `ResolveTernaryArmClasses` builds `armTypes` POSITIONALLY parallel to the
phi's incoming values, and `JoinTernaryArmPredicates` folds one i1 answer per arm. A nested arm has
no single class - it has a set of leaf classes with different answers - so the fix is not a
recursion into a name lookup but a per-leaf predicate joined at the INNER join point and fed into
the outer fold. That is a different change shape with its own IR questions.

## Site 2 - `BoxTernaryThinArmToInterface` in `cflat/MainListener.h`, the MIXED fat/thin ternary

```cflat
extern int main()
{
    Circle* a = new Circle(); a->r = 3;
    Circle* c = new Circle(); c->r = 4;
    Circle* z = nullptr;
    IShape fat = c;
    IShape j = ib(false) ? fat : (z ?? a);   // one arm already fat, the other a '??' join
    return j.area();
}
```

```
s_thin_arm_chain2.cb(5,29): cannot convert '?:' arm to interface 'IShape': the arm's concrete
class cannot be determined; bind the arm to a local variable of the class type first
```

Identical on both binaries. Why it was not folded in: the caller positions the builder in the thin
arm's OWN block before calling, but a nested join's fat phi belongs in the NESTED join's resume
block, not the caller's block - so the fix has to interact with `UnifyTernaryArmTypes` and the
mixed fat/thin unification path, which the chain fix's coverage matrix never enumerated.

## Fix direction

Both are the same root as the fixed sites - `ResolvePointerElementTypeName` cannot name a join -
but neither is a drop-in recursion. Take them one at a time, and build the accept-set FIRST: both
sites currently REJECT, so the hazard is a false accept that boxes or tests the wrong arm, which
is silent. `CollectPointerJoinArms` / `NestedJoinArmsBoxable` (`cflat/MainListener.h`) are the
reusable pieces.

## Related

[[as-is-does-not-recognize-nullcoalesce-join]], [[interface-issue-queue]]
