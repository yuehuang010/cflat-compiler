# 'if const (<const global>)' at file scope corrupts an already-emitted unrelated function

Filed 2026-07-29, found while building corpus legs for
`generic-interface-registered-as-opaque-struct.md`. **Pre-existing on master** (verified against a
pre-fix binary) and unrelated to the generic-interface work - it just happens to be the shape you
reach for when you want an `if const` condition that a source-only pass cannot fold.

Severity: LLVM verifier failure reachable from plain source, and the corrupted function is one the
user never wrote near the `if const`.

## Repro

```cflat
import "test_helper.cb";
const int GI_NEVER = 0;
if const (GI_NEVER) {
    class Vetoer<T> { T junk = default; };
}
extern int main()
{
    printf("bis9\n");
    return 0;
}
```

Identical on both binaries:

```
Module verification failed:
Basic Block in function '_Test_int_charPtrstringstring_' does not have terminator!
label %ifResume
```

`Test(...)` is the assertion helper in `Test/test_helper.cb`, emitted long before this `if const`
is reached. Its `%ifResume` block is left unterminated.

Notes:

- The body of the `if const` is irrelevant - it does not have to contain a template.
- `if const (sizeof(int) == 8)` on the same file is fine, as are `if const (__MACOS__)` and the
  other compile-time-macro forms, so the trigger is specifically a **const-global load** in the
  condition.
- A file-scope `if const` is required; the member-scope and statement-scope forms were not probed.

## Root cause direction

`MainListener::DecideIfConstCondition` -> `EvalIfConstConstant` -> `EmitAndFoldIfConstLeaf` is
documented to route every leaf "through a discarded scratch function ... so a global or enum load
cannot crash on a null insert block", and it takes a `forceScratch` flag for exactly that. The
file-scope `ParseIfConstDeclaration` call site passes `forceScratch=false`
(`DecideIfConstCondition` hard-codes it), so the const-global load is emitted into whatever insert
block the builder was last left in - here, the middle of the previously-emitted `Test` helper -
and the branch/phi lowering for the load abandons `%ifResume` without a terminator.

## Fix direction

Either pass `forceScratch=true` from the file-scope `if const` path (there is no live function at
file scope, so a scratch function is always correct there), or save/restore the builder insert
point around `DecideIfConstCondition` the way the tuple path does with
`SaveBuilderState`/`RestoreBuilderState`. Then add a regression leg to `Test/test_basic.cb`'s
`testIfConst()` family using a `const int` global condition at FILE scope - the existing legs there
use literals and macros, which is why this was never caught.

## Repro file

`scratch/gi/bis9.cb`.
