# `char* + string` emits invalid IR (GEP with a string index) instead of a compile error

Filed 2026-08-25, found during Windows verification of the ui-native controls tiers.

## Repro

```cflat
extern int main()
{
    string name = "world";
    string s = (true ? "a " : "b ") + name;   // ternary of literals -> char*, then + string
    printf("%s\n", s.data());
    return 0;
}
```

```
Module verification failed:
GEP indexes must be integers
  %ptrarith = getelementptr i8, ptr %ternary, %string %592
Error: module verification failed.
```

The direct spelling `char* p = "a "; string s = p + name;` should reproduce the same way -
the trigger is a left operand typed `char*` (here from a ternary joining two string
literals) with a right operand of struct type `string`.

## Root cause

Binary `+` with a pointer LHS routes to pointer arithmetic; the RHS is passed through as
the GEP index without checking it is an integer. A `string` (struct) RHS reaches
`CreateGEP` and only the LLVM verifier catches it, as an ICE-grade failure instead of a
diagnostic.

## Fix direction

In the binary-operator path, before emitting pointer arithmetic, check the index operand
is an integer type; if not, `LogError` ("cannot add a value of type 'string' to a
pointer; convert the pointer to a string first, e.g. \"\" + ptr") per the LLVM-assert
convention in CLAUDE.md. Deciding to instead make `char* + string` concatenate is a
separate language ruling; the error is the safe first step.

## Workaround

Bind the ternary/pointer to a `string` local first: `string p = cond ? "a " : "b ";
string s = p + name;` (this was applied in `cflat/core/ui_native/win32.cb`).
