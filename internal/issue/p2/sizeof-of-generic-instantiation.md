# 'sizeof(Generic<T>)' does not mangle its type arguments - "unknown type 'B<int>'"

Filed 2026-07-29, split out of the generic-interface registration work (`09f1d56`, design record in `interface-issue-queue.md`). That issue
listed `sizeof(C<int>)` on a generic INTERFACE as part of its accept set, but the gap is not
interface-specific at all and is unchanged by that fix.

## Repro - a plain generic STRUCT is enough

```cflat
import "test_helper.cb";
struct B<T> { T v = default; };
extern int main()
{
    B<int> b = default;
    printf("%d\n", (int)sizeof(B<int>));
    return 0;
}
```

```
z_sizeof.cb(6,24): unknown type 'B<int>'
```

Identical on the pre-fix and post-fix binaries, and identical for a generic interface
(`scratch/rev/g01_sizeof.cb`). The declaration `B<int> b` on the line above resolves fine, so
only the `sizeof` operand path is affected.

## Root cause direction

The `sizeof` operand is resolved as a raw type NAME (`typeName`/`getText()`) and handed to
`GetType` without going through the generic-instantiation mangling that
`ParseDeclarationSpecifiers` performs (`Base<Args>` -> `Base__Args`, plus
`QueueGenericInstantiation`). `GetType` then looks up the literal spelling `"B<int>"`, which is
in neither `dataStructures` nor `interfaceTable`, and reports `unknown type`.

Likely the same gap applies to any other position that takes a bare `typeName` rather than
`declarationSpecifiers` - `alignof`, `typeof`-style builtins and cast operands are worth checking
in the same pass.

## Fix direction

Route the `sizeof`/`alignof` operand through the same generic mangling + queue path
`ParseDeclarationSpecifiers` uses, so `sizeof(B<int>)` resolves `B__int` (and instantiates it if
this is its first use). Add a regression leg to `Test/test_generics.cb` covering a generic struct,
a generic class, and a generic interface operand.

## Repro files

`scratch/rev/g01_sizeof.cb` (interface form, read-only review evidence),
`scratch/gi/z_sizeof.cb` (plain generic struct form).
