# Tuple type sugar `(A, B)` does not compile inside a namespace

Filed 2026-07-30, found by the adversarial review of
[[interface-issue-queue]] (landed design records) (round 2). **Pre-existing on both binaries.**

Severity: **false rejection.** A whole syntax is unavailable inside a namespace.

## Repro

`scratch/rev5/s1d_tuple_int_ns.cb` - PRIMITIVE elements, no ambiguity, nothing generic written:

```cflat
namespace A
{
    int f() { (int, float) p = default; return (int)p.Item0; }
}
```

```
s1d_tuple_int_ns.cb(6,18): type 'tuple__int__float' has an incomplete layout (a field type C interop
could not import); it can only be used through a pointer
```

Identical on `15809e0` and on the type-argument fix. The EXPLICIT spelling of the same type,
`tuple<int, float>`, works in the same position (`scratch/rev5/s1e_tuple_explicit_only.cb`), so this
is specific to the `( , )` sugar. Also reproduced with struct elements
(`scratch/rev5/s1_tuple_sugar.cb`, `s1b_tuple_sugar.cb`).

The diagnostic is the generic opaque-shell message reused for an incomplete layout of any cause, and
it blames C interop on a file that imports no C - the same misleading message recorded under T5 of
[[interface-issue-queue]] (landed design records).

## Root cause direction

Not diagnosed. The tuple-sugar paths pre-declare the shell and queue the instantiation from four
places (`ForwardRefScanner::ParseDeclarationSpecifiers`, `ScanGenericTypeUses`,
`MainListener::ParseDeclarationSpecifiers`, `QueueInstantiateGenericType`), and the instantiation of
the `tuple` template itself is guarded by `genericStructTemplates.count("tuple")`. The shell is
created ("incomplete layout" means the type EXISTS but has no body), so the failure is that nothing
drains its instantiation when the declaration sits inside a namespace - the same
shell-created-but-never-completed family as the base-name bug that `15809e0` fixed, reached by the
tuple route. `tuple` is a core template at global scope, so the suspect is the namespace context
present when the sugar queues versus when the drain looks the instantiation up.

## Consequence for the type-argument fix

`generic-type-arguments-not-key-space-resolved` routes tuple ELEMENT names through the same
namespace walk as any other generic type argument, so that `(Item, int)` and `tuple<Item, int>`
cannot mangle to two different types inside a namespace. That half of the change is currently
**unexercisable** - the sugar does not compile in a namespace at all - and is retained only so the
two spellings stay in agreement the moment this issue is fixed. The observable effect today is
limited to the mangled name inside the error message
(`tuple__Item__int` -> `tuple__A.Item__int`, `scratch/rev5/s1b_tuple_sugar.cb`). Whoever fixes this
issue must re-check that the two spellings still name one type.

Related: [[interface-issue-queue]] (landed design records)
