# A `program`'s struct-typed field with NO initializer is zeroed, not default-constructed

## Summary

`ParseProgramDefinition`'s synthesized default constructor
(`MainListener_Aggregates.cpp:2220-2275`) seeds brace-list defaults, `= expr` defaults and
`= default`, but it has no arm for "no initializer at all on a struct-typed field". The struct
and class emitters both have that arm (`:326-335` and `:2775-2784`): they call the field type's
own default constructor so the field's own field initializers run. The program emitter does not,
so such a field reads as all zeros - a silent wrong value that differs from the identical
declaration inside a `struct` or `class`.

## Repro

```cflat
import "list.cb";
struct Deep { int w = 42; };
struct Inner { int v = 7; Deep d; };
program P
{
    Inner nested;          // no initializer  -> WRONG: zeroed
    Inner dn = default;    // '= default'     -> correct: 7 / 42
    int plain = 5;
    int main(move list<string> args)
    {
        printf("nested.v=%d nested.d.w=%d dn.v=%d dn.d.w=%d plain=%d\n",
            nested.v, nested.d.w, dn.v, dn.d.w, plain);
        return 0;
    }
};
extern int main()
{
    P p;
    list<string> args;
    p.run(args);
    p.WaitForExit();
    return 0;
}
```

Measured (macOS arm64 Release, `-i Test/library`), identical on master (7037b95) and on
fix/defctor:

```
nested.v=0 nested.d.w=0 dn.v=7 dn.d.w=42 plain=5
```

Expected `nested.v=7 nested.d.w=42`, which is what the same fields produce in a `struct` or
`class` (`scratch/fx3_s_none.cb`, `scratch/fx3_c_none.cb`: `nested.v=7 nested.d.w=42`).

Probe: `scratch/fx3_program_field.cb`.

## Root cause

`MainListener_Aggregates.cpp:2253` walks the built `initializers` vector and only acts when
`rvalue != nullptr`. The struct/class emitters instead test `rvalue == nullptr &&
destType->isStructTy()` first and fill it with `CreateOverloadedFunctionCall(fieldTypeName, {},
true)` (forceRoot, guarded by an exact-key `GetFunction` lookup). That block is simply absent
from the program emitter.

## Fix direction

Copy the struct emitter's `rvalue == nullptr && destType->isStructTy()` arm into
`ParseProgramDefinition`'s insert loop, before the `if (rvalue)` test, keeping the `forceRoot`
`GetFunction` guard. The synthetic program fields (`exitCode`, `_thread`, `_allocator`,
`onStdout`, ... ) are all written explicitly AFTER that loop, so they are unaffected.

Not affected: `ParseImportedProgramDefinition` (`:1882-1935`). `import program "x.cb"` wraps a
standalone file whose entry point is a free `main`; the program body has no user field list at
all, so that emitter's `declList` really is entirely synthetic (its in-code comment says so, and
the import path rejects a file without a free `main`). Its identical missing arm is dead.
