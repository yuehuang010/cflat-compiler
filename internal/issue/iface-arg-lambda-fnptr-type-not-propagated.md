# Interface call arguments do not propagate `lastLambdaType` (no repro found)

Filed 2026-07-28 during the interface named-argument field-parity audit. Recorded as a KNOWN
GAP with no demonstrated symptom, so a future investigator does not repeat the search.

Severity: none observed. Filed rather than fixed because every fix here is speculative and
would change interface overload SCORING.

## The gap

The direct call path ends its argument loop with (`MainListener.h` ~:20505):

```cpp
if (lastLambdaType.IsFunctionPointer && argValue != nullptr)
{
    argVar.TypeAndValue = lastLambdaType;
    argVar.LambdaCaptureNames = Compiler(ctx)->lastCallLambdaCaptureNames;
    lastLambdaType = {};
    Compiler(ctx)->lastCallLambdaCaptureNames.clear();
}
```

The interface arm (`MainListener.h` ~:19945) has no counterpart, so a lambda argument reaches
`ResolveInterfaceMethodSlot` typed only by its LLVM struct name (`__closure_fat_ptr`), losing
`FuncPtrParams` / `FuncPtrReturnTypeName`.

## What was tried

1. **Stale-side-channel leak.** `lastLambdaType` is a MainListener member and the interface arm
   never clears it, so a lambda passed through an interface call could in principle leak its
   function-pointer type into the NEXT direct call's argument (the failure mode
   `Test/test_function_ptr.cb:866` documents for an earlier bug). Repro attempted at
   `scratch/f4d_lambdatype_leak.cb`: an interface call taking `Lambda<int(int)>` immediately
   followed by `n.plain(7)`. Prints `plain=21` - correct. Postfix processing clears the channel
   before the next call site reads it (see the note at `LLVMBackend.h:1169`), so the leak does
   not survive.

2. **Overload scoring between closure-typed slots.** Two same-arity interface slots taking
   different closure signatures would need `FuncPtrParams` to be ranked apart. Not pursued:
   `ComputeOverloadFunction` ranks on `TypeName`, and both slots present an encoded closure
   type name that the argument (`__closure_fat_ptr`) matches equally well either way, so
   propagating the type does not obviously break the tie. Blocked in practice by
   `iface-thin-function-param-no-lowering.md` for the thin `function<>` spelling.

## Fix direction

Do not copy the field speculatively. Wait for a shape where interface slot selection is
provably wrong, then propagate `lastLambdaType` in the interface arm exactly as the direct
path does - including the clear, so the side channel is retired by whichever arm consumes it.
