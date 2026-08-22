# A ternary (or `??`) of two string literals slips the literal-into-struct-pointer guard

Filed 2026-08-21 during review of the string-literal-into-struct-pointer fix
(the "Close three crash/UAF issues" commit, `LLVMBackend::IsStringLiteralIntoStructPointer`). Measured on the
current tree, macOS arm64, Release.

## Repro (compiles clean, misinterprets the string bytes as struct fields at runtime)

```cflat
struct Q { int a = default; int b = default; };

extern int main()
{
    bool g = true;
    Q* q = g ? "aaaa" : "bbbb";   // both arms are literals; guard does not fire
    return q.a;                   // reads the literal's bytes as an int
}
```

`Q* q = "aaaa";` alone is correctly rejected by `RejectStringLiteralIntoStructPointer` at all
eight store sites added by that commit. The ternary spelling above reaches the same declarator
store site but is accepted.

## Root cause

`IsStringLiteralIntoStructPointer` proves the reject by asking whether the incoming
`llvm::Value*` is itself an `llvm::Constant` that `IsStringLiteralConstant` recognizes
(`LLVMBackend_CodegenHelpers.cpp`). A ternary's result is not that constant directly - it is
a `phi` (or a materialized temporary) joining the two arm values, so the direct-constant test
sees a non-constant value and never fires, even though every possible value the phi can take
is a string-literal pointer. The guard tests the literal, not a join of literals; the same gap
applies to `??` for the same reason (a coalesce also joins two operand values without preserving
either as a directly-visible constant to the caller).

## Fix direction

At the point each of the eight `RejectStringLiteralIntoStructPointer` / `IsStringLiteralIntoStructPointer`
call sites has access to the *source expression* (not just the lowered value), recognize a
ternary (`?:`) or coalesce (`??`) whose every arm is itself a string-literal expression (recursing
through nested ternaries/coalesces) and treat that source shape as a literal for the guard -
i.e. reject it the same way a bare literal is rejected, with the same diagnostic. This needs the
check pushed up to the expression-shape level (where the ternary/coalesce arms are still
individually visible) rather than done purely on the final lowered `llvm::Value*`, since by then
the arms have already been joined into a single SSA value indistinguishable from a real runtime
`char*`.

Affected sites (mirror the list documented in that commit's message): declarator init, `=`
assignment, brace-init member, field default, fixed-array element, array-view element, return,
call argument / default parameter.
