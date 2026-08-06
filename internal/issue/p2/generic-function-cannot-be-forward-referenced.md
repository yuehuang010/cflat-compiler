# A generic FUNCTION cannot be called above its definition; the diagnostic invents a candidate

Filed 2026-08-05 from the `fix/genfp-return` coverage matrix (the "instantiation axis" cell
"producer defined AFTER the caller"). Pre-existing; **identical on the pre- and post-fix binaries
of that branch**, measured in this exact spelling. Not a `function<>` issue - it reproduces with a
plain `int` template.

## Repro

```cflat
int caller() { return idg<int>(4); }
T idg<T>(T v) { return v + 1; }
extern int main() { printf("%d\n", caller()); return 0; }
```
```
z9.cb(1,22): no overload of 'idg__i32' matches the given arguments.
  Call arguments (1):
    [0] i8 <unnamed>
  Candidates (1):
    _idg__i32_idg__i32__()
```

Moving the template above `caller` compiles and runs (prints 5). A NON-generic function does not
need this - `ForwardRefScanner` pre-registers ordinary signatures, which is the whole point of the
pre-pass - so the generic path is the odd one out.

## What the message shows

Two things beyond the missing capability, both cosmetic but both actively misleading:

- The sole "candidate" `_idg__i32_idg__i32__()` is zero-parameter and does not exist. It is the
  same phantom described as defect 1 of [[generic-function-call-diagnostics-are-misleading]] -
  the mangled call name echoed back through the overload printer.
- The call argument is printed as `i8 <unnamed>` for a literal `4`.

That file covers the message for an UNDECLARED template. This one is about a template that IS
declared, later in the same file, so the message is wrong about the program as well as ugly.

## Fix direction (not diagnosed)

`ForwardRefScanner` collects generic STRUCT/CLASS/INTERFACE template names for the whole TU before
scanning uses (`CollectGenericTemplateDecls` / `ScanGenericInterfaceTemplateNames`,
`cflat/MainListener.h:2830`, driven from `cflat/LLVMBackend.cpp:788-790`). Establish whether the
generic FUNCTION templates get an equivalent whole-TU collection pass, and if not, whether one can
be added on the same ordering guarantee. Check the `certain=false` trap the
[[unresolved-generic-preregisters-opaque-shell]] file documents (`if const` arms and
`expect_error` blocks are deliberately not collected) before making anything depend on the
registry being complete.

## Related

[[generic-function-call-diagnostics-are-misleading]],
[[unresolved-generic-preregisters-opaque-shell]], [[interface-issue-queue]]
