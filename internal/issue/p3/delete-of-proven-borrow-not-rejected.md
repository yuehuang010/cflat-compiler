# delete of a proven-borrow pointer is not rejected at compile time

Bucket: q01 (ownership), one-site candidate once the align-doors provenance lands.

## Summary

`delete p;` where `p` was initialised from `&stackLocal` compiles clean (`--check`, `-o`) and
frees a stack address at runtime. Native exe dies with SIGABRT from the allocator; under `--run`
the in-process abort is caught by the CompilerManager crash handler and prints a compiler state
dump, which looks like a compiler crash but is the program's runtime abort.

## Repro (found 2026-09-04 while fixing the ternary-join issue, master e6f3521)

```cflat
struct Node { int v = 3; };
extern int main(int argc, char** argv)
{
    Node stack = default;
    Node* borrowed = &stack;
    delete borrowed;        // compiles; aborts at runtime
    return 0;
}
```

`x64/Release/cflat probe.cb --run` -> rc 134 with "=== abort() called - compiler state dump ===".

## Root cause

The delete door checks the operand's ownership only through IsOwning / move state; a pointer
whose provenance is a stack address-of is a plain borrow and passes. Commit 3860071
(fix/align-doors) adds `NamedVariable::PointsToBorrowedAddress`, set at the declaration and
assignment doors, exactly the fact needed here.

## Fix direction

At the `delete` statement door (MainListener, the site that emits the deallocator call): if the
operand is a NamedVariable with PointsToBorrowedAddress set, LogErrorContext
"cannot delete '{}': it borrows the address of a stack variable" (wording open). Provenance on
NamedVariable, not IR sniffing. Extend Test/errors/err_ternary_mixed_ownership.cb or the existing
delete error file rather than adding a new file unless the message is new. Accept-set: delete of
a pointer that was reassigned from `new` after the address-of must still compile (door must clear
the flag on reassignment - the align-doors change already does).

Separately: the `--run` crash handler catching the PROGRAM's abort and printing a compiler state
dump is misleading; consider marking the dump "program aborted (not the compiler)" when the
abort arrives after codegen finished. Small, ui-bucket-sized.
