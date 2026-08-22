# Residual: `sizeof(a<b,c>d);` is still read as a type - only the WORDING is fixed

Filed 2026-08-06; wording fixed in the p3 bundle (off `819848e`), behaviour still deferred.

## What changed

The message no longer invents a mangled type name. Measured on this branch:

| Program | Before | Now |
|---|---|---|
| `sizeof(a<b,c>d);` (discarded) | `cannot find the type 'a__b__c'` | `'a<b,c>d' is not a type name; the operand of 'sizeof' is read as a TYPE here, so a comparison or tuple expression is not accepted - bind it to a variable first and measure that` |
| `i64 z = sizeof(a<b,c>d);` | same mangled message | same new message |
| `sizeof(Pair<int,float>)` | 8 | 8 (unchanged) |

The guard sits in the `typeName` arm of `MainListener_Expressions.cpp` (before
`GetType(elementTV, ...)`), keyed on name characters TRAILING the closing `>` - a shape no type
spelling has. Both statement forms of the repro reach that arm. The prefix-`sizeof` arm
(text-reconstructed operand) is NOT affected: it only engages when the generic base is a known
template, and `sizeof(Pair<int,int>x)` compiles silently there rather than erroring, so there is
no message to improve. Regression leg: `Test/errors/err_types.cb`.

## What is left (unchanged)

The spelling is still STOLEN: a discarded `sizeof(a<b,c>d);` is an error, not the no-op tuple
comparison the writer meant (it already errored before this bundle - only the wording changed). Restoring it still requires routing the prefix-`sizeof` operand through the real
`typeName` rule so the PARSER, not a character test, settles the type-vs-tuple ambiguity; the
naive "fall back to the expression path" repair walks back into the null-`Primary` SIGSEGV that
`fix/sizeof-closure` closed. Severity is unchanged (a discarded no-op), so this stays deferred.
