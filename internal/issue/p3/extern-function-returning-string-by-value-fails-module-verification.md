# An `extern` function that RETURNS `string` by value fails module verification

## Summary

Defining an `extern` (C-ABI) function whose return type is `string` emits a function whose LLVM
return type comes from the extern ABI recipe (`{ i64, i64 }`) while the `return` site emits the
`%string` value, so the module fails verification and nothing compiles.

Pre-existing and unrelated to `alias`: the non-alias spelling fails identically, and the `alias`
form fails only because it shares the same extern return lowering.

## Repro

```cflat
string g_s = default;
extern string f() { return g_s; }
extern int main() { g_s = "hello".copy(); printf("%s\n", f().data()); return 0; }
```

```
Module verification failed:
Function return type does not match operand type of return inst!
  ret %string %borrow.str
 { i64, i64 }
```

`extern alias string f()` fails the same way. `extern Row f()` (plain struct), `extern int f()`
and the non-extern `string f()` / `alias string f()` all compile and run.

## Root cause

`CreateFunctionDeclaration` computes an `AbiRecipe` for an `external` function and, when the
recipe has lowering, builds the type with `BuildExternFunctionType` (coerce-to-int for a
16-byte struct return). `CreateFunctionDefinition` always uses `GetFunctionType`, which for the
extern path returns `GetCCompatibleType(returnType)` = `%string`. The definition and the
declaration therefore disagree, and the `return` emission follows the definition.

## Fix direction

Route `CreateFunctionDefinition` through the same recipe path as `CreateFunctionDeclaration` for
an `external` function, and have `EmitReturnExpression` apply the recipe's return coercion when
one is in force. Either that, or reject an `extern` definition whose return type needs a recipe
with a real diagnostic instead of an LLVM verifier failure.
