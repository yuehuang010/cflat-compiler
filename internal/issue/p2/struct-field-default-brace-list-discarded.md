# A struct FIELD's own `= { ... }` default brace list is silently discarded

Filed 2026-08-02, found by review while auditing the global-struct-positional-init
family for neighbouring shapes. Different root cause (this fires on a struct
FIELD's default value, not a variable declarator's initializer - no `S x = {...}`
or `S x {...}` anywhere in the repro), kept as its own item, not fixed here.

Severity: silent wrong value.

## Repro

Measured on the POST binary (state after the global-struct-positional-init fix,
including its bare-brace-spelling round; this path is untouched by that fix):

```cflat
struct Inner { int x; int y; };
struct Outer { Inner i = { x = 1, y = 2 }; int z; };
extern int main(){ Outer o; printf("x=%d y=%d z=%d\n", o.i.x, o.i.y, o.z); return 0; }
```

-> compiles rc 0, runs rc 0, prints `x=0 y=0 z=0` (expected `x=1 y=2 z=0`: `i`'s
own brace-list default should seed it to `{1,2}`, `z` correctly defaults to `0`
since it has no field default expression).

## Root cause

Not diagnosed. Likely lives in `GenerateDefaultValue` (`MainListener.h` ~line
4221) or wherever a struct's per-field default expressions are folded into the
struct's own default-Constant: `Outer`'s default value needs to recursively
evaluate `Inner`'s brace-list default (`{x=1,y=2}`) rather than falling back to
`Inner`'s all-zero default. This is architecturally close to the sibling
`internal/issue/p3/global-struct-no-initializer-ignores-field-defaults.md`
(also about field defaults not being honored) but that one is about a variable
with NO initializer at global scope; this one is about the FIELD's own default
expression being a brace list, and reproduces at local scope with an
explicitly-declared local (`Outer o;`), so the two are not obviously the same
code path - re-diagnose rather than assuming they share a fix.

## Fix direction

Not diagnosed to a specific plan. Trace what `GenerateDefaultValue` does for a
field whose `DeclTypeAndValue` carries a brace-list default (vs. a scalar
default like `int a = 9;`, which IS honored per the neighbouring `field_init_default`
leg in `Test/test_basic.cb`) and find where the brace list is dropped instead of
being applied on top of the field's own zero/default value.
