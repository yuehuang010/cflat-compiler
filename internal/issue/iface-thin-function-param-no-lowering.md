# Interface method with a thin `function<>` parameter: module verification failure, no diagnostic

Filed 2026-07-28 while auditing which argument fields the interface call arm fails to copy.
PRE-EXISTING and unrelated to named arguments - the positional spelling fails identically.

Severity: compile aborts with a raw LLVM verifier dump and no source location. A legal
non-capturing lambda is rejected; a capturing one loses its specific diagnostic.

## Repro

```cflat
import "function.cb";
interface IF { void run(function<int(int)> fn); };
class N : IF { void run(function<int(int)> fn) { printf("run=%d\n", fn(5)); } };

extern int main()
{
    N n;
    IF io = n;
    io.run((int x) => { return x * 4; });   // NON-capturing, positional
    return 0;
}
```

```
Module verification failed:
Call parameter type does not match function signature!
%__closure_fat_ptr { ptr @___lambda_0_int_intU8Ptr_, ptr null }
 ptr  call void %9(ptr %7, %__closure_fat_ptr { ... })
```

The minimal repro above emits only the `Call parameter type` error. Some larger
shapes additionally report an `Invalid bitcast`; that is a downstream consequence of
the same missing lowering, not a second defect.

The capturing variant (`return x * factor;`) fails the same way. The DIRECT path handles
both correctly - it accepts the non-capturing lambda and rejects the capturing one with:

```
cannot pass to C function-pointer parameter 'fn': this lambda captured 1 variable [factor].
```

`scratch/adv20_lambda_reorder.cb` and `scratch/adv21_lambda_positional.cb` are the same
shape (the positional control fails identically, which is what proves this is not a
named-argument regression). The `Lambda<int(int)>` spelling - a capturing owning closure -
works on the interface path in both orders (`scratch/adv22_lambda_iface.cb`).

## Root cause

`CreateOverloadedFunctionCall` (`LLVMBackend.h` ~:16895) lowers a closure fat struct to a
bare C function pointer when the param is a thin `function<>`: it extracts field 0 if the env
field is a compile-time null, and otherwise reports the capture list from
`NamedVariable::LambdaCaptureNames`. `CallInterfaceMethod`'s argument loop
(`LLVMBackend.h` ~:12566) has no equivalent - the scalar/by-value branch hands the whole
`%__closure_fat_ptr` to a slot typed `ptr`, so the verifier rejects the call.

Two things are missing, in this order:

1. The fat-to-thin lowering itself, in `CallInterfaceMethod`'s by-value branch.
2. `argVar.LambdaCaptureNames = argNV.LambdaCaptureNames;` in the interface argument loop
   (`MainListener.h` ~:19985), so step 1 can name the offending captures. Copying the field
   alone fixes nothing - there is no consumer on this path until step 1 exists, which is why
   it was left out of the field-parity pass.

## Fix direction

Factor the fat-to-thin conversion out of `CreateOverloadedFunctionCall` into a helper on
`LLVMBackend` (it needs only `builder`, the value, and the target LLVM type) and call it from
`LowerByValueArg`, which both paths already share - that fixes the interface path and any
future caller at once. Then copy `LambdaCaptureNames` onto the interface argument so the
capture diagnostic reads the same on both paths.
