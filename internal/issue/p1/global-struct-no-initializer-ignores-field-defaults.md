# A global struct with NO initializer ignores its fields' `= default` expressions

Filed 2026-08-02 while enumerating the coverage matrix for the (now fixed)
global-struct-positional-init-silently-zeroes issue. Different root cause
(this is the `right == nullptr`, no-initializer-at-all path, not the
brace-list path that issue fixed), kept separate.

Severity: silent wrong value, lower impact than the sibling issues in this
family (the local declarator handles this correctly, so there is a working
spelling - `struct S s; ...;` inside a function - callers can reach for).

## Repro

Measured on the pre-fix binary built from `58d5d27` and unaffected by the
sibling struct-positional-init fix (different code path):

```cflat
struct S { int a = 9; int b = 9; };
S gs;
extern int main(){ printf("a=%d b=%d\n", gs.a, gs.b); return 0; }
```

-> compiles rc 0 (prints a `LogWarningContext` note to stdout: `(S) struct and
class is not initialized on the stack.`), runs rc 0, prints `a=0 b=0` (expected
`a=9 b=9`, the fields' own default expressions).

The identical LOCAL spelling (no initializer, inside a function) is correct:

```cflat
struct S { int a = 9; int b = 9; };
extern int main(){ S ls; printf("a=%d b=%d\n", ls.a, ls.b); return 0; }
```

-> prints `a=9 b=9` (correct - local uses the type's constructor/default path).

## Root cause

`MainListener.h` around line 9520-9530: when `right == nullptr` (no initializer
at all) and the type is a known struct, the LOCAL branch calls
`compiler->GetFunction(typeAndValue.TypeName)` to invoke the type's (possibly
compiler-synthesized) default constructor, which seeds field defaults. The
`global_scope` branch instead just falls into `LogWarningContext(...)` and
leaves `right` null, which `CreateGlobalVariable` then turns into a plain
`zeroinitializer` - the field default expressions are never evaluated.

## Fix direction

Not diagnosed to a specific plan. The global path would need to either call
the same default-constructor machinery the local path uses (if that can
produce a compile-time Constant instead of runtime instructions - field
defaults that are themselves constant expressions likely can be folded; a
default constructor with non-constant logic could not), or the existing
`LogWarningContext` note should be upgraded to make clear the fields are being
force-zeroed regardless of their own defaults. Sweep whether `GenerateDefaultValue`
(used by the brace-list path) already honors field defaults - if so, routing
the no-initializer case through it (instead of leaving `right` null) may be a
small, low-risk fix.
