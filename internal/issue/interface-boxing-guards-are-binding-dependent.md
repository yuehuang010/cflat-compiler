# Interface boxing guards are binding-dependent: parens or a `?:` join defeat them

Filed 2026-07-29 by the review of the boxing consolidation (`BoxConcreteIntoInterface`).
PRE-EXISTING and NOT a regression: every shape below behaves identically on the pre-change
binary. The consolidation closed the guard gap for a NAMED source; these are the shapes
that arrive with no name.

Severity: DOUBLE FREE (exit 134 on macOS without asan). Silent in a build whose allocator
does not check.

## Repro

Both spellings, both freeing twice:

```cflat
interface IShape { int area(); };
class Circle : IShape { int r = 0; int area() { return r * r; } ~Circle() {} };

extern int main()
{
    Circle* c = new Circle(); c.r = 2;
    IShape s = (c) as IShape;     // parens: exit 134
    printf("paren=%d\n", s.area());
    delete s;
    return 0;
}
```

`IShape s = (c);` - the plain spelling with the same parentheses - is exit 134 too.
Removing the parentheses fixes both: `IShape s = c as IShape;` and `IShape s = c;` are
exit 0, because the source is then a named binding and the transfer runs.

A `?:` join of two owning arms is the same failure with no parentheses needed on the
operator itself:

```cflat
Circle* a = new Circle(); a.r = 2;
Circle* b = new Circle(); b.r = 3;
IShape s = c > 0 ? (a) : (b);     // exit 134
delete s;
```

## Root cause

The whole guard family keys off the source `NamedVariable` (`srcNV` in
`BoxConcreteIntoInterface`, `MainListener.h:9969`). The ownership transfer - store null
into the source storage, `MarkVariableMoved`, `MarkVariableMovedIntoInterface` - can only
run when there IS a binding to null.

`SoleCastOperandOf` recovers the binding only through a pure single-child passthrough
chain down to `castExpression`. A parenthesized operand is a primary expression wrapping a
full expression, so the walk does not reach the cast level and returns null; a `?:` join
has two sources and no single binding at all. In both cases the box is built correctly but
the source keeps its owning flag, so `delete <iface>` plus the source's scope-exit free
frees twice.

## Fix direction

Do not extend `SoleCastOperandOf` to see through parentheses as a point fix - that closes
the paren spelling and leaves the `?:` join, and the next shape that erases a binding
reopens it. The durable answer is the one the consolidation started: make ownership follow
the VALUE rather than the NAME.

The provenance ledger added alongside `BoxConcreteIntoInterface`
(`LLVMBackend::interfaceBoxRecords_`, keyed on the fat value and its data half) already
records `Source` and `OwnershipTransferred` per box. Extend it so a box whose source was
an owning value - however that value was spelled - retires the ORIGINAL owning temp,
rather than requiring a named local to null. `RegisterOwnedPtrTemp` / `IsOwningPtrTempValue`
(`LLVMBackend.h:2142`) are the existing value-keyed ownership machinery to build on.

For the `?:` join specifically, `UpcastTernaryPhiToInterface` (`MainListener.h:10662`)
boxes per arm and is the natural place to transfer per arm.

## Related

[[interface-boxing-sites-not-fully-consolidated]], [[interface-issue-queue]]
