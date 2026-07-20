# `if const` with a global-variable condition segfaults

## Summary

An `if const` whose condition reads a global variable crashes the compiler (SIGSEGV)
instead of producing the "must be a compile-time constant expression" diagnostic.
Pre-existing at file scope; the new member-scope form inherits it.

## Repro

```cflat
int flag = 1;
if const (flag) { int a = 1; }     // file scope - segfault
extern int main() { return 0; }
```

```cflat
int flag = 1;
class Bad
{
    if const (flag) { int a = 1; }  // member scope - same segfault
};
extern int main() { return 0; }
```

Both crash with exit 139 under plain compile and under `--check`.

## Root cause

`ParseIfConstDeclaration` / `AppendIfConstMembers` call `ParseExpression` on the condition
while the walk is at declaration scope, where the IR builder has no insert block. Constant
conditions (`__MACOS__`, `is_unique(T)`, ...) fold without emitting, so they are fine. A
global read must emit a load, and the builder dereferences a null insert block.

A condition that is a *function call* does not crash - it reports the correct diagnostic -
but leaves a half-built block behind, so the scoped-block `expect_error(...) { ... }` form
then fails module verification. Only the bare-semicolon `expect_error(...);` form works
for these tests (see `Test/errors/err_if_const_member_nonconst.cb`).

## Fix direction

Evaluate the `if const` condition with the builder pointed at a scratch function that is
discarded afterwards, or detect a null insert block inside the expression path and report
"'if const' condition must be a compile-time constant expression" instead of emitting.
The same fix removes the leftover-terminator problem for the call form.
