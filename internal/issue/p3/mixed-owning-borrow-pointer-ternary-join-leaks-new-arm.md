# Mixed owning/borrow POINTER '?:' join leaks the `new` arm

## Summary

`Node* p = h ? borrowed : new Node();` compiles clean and leaks the 16-byte `Node` whenever the
`new` arm is taken (`leaks --atExit`, macOS Release, measured 2026-09-03 on master c56efdf and on
the ternary-ownership branch). This is the pointer-typed sibling of the owning-value STRUCT join
that e4e493a turned into a compile error under the 2026-09-03 ruling; the ruling text ("one arm an
owning value or temporary, the other a borrow of an existing value") covers this shape too, but it
was not widened without measurement.

## Repro

```cflat
struct Node { int v = 3; };
extern int main(int argc, char** argv)
{
    Node stack = default;
    Node* borrowed = &stack;
    Node* p = (argc > 1) ? borrowed : new Node();   // new arm leaks when taken
    printf("%d\n", p->v);
    return 0;
}
```

## Fix direction

Extend RejectMixedOwnershipTernaryJoin (cflat/MainListener_Declarations.cpp) to pointer joins
where one arm is a fresh `new` / `move` / owning call result and the other is a borrow, with the
same diagnostic. Check doc/LANGUAGE.md for the shipped pointer/interface mixed-join trade before
widening: if it is documented as accepted-and-borrowing, the doc changes with the rule. Blast
radius: grep Test/ and example/ for `? new ` and `: new ` ternaries.
