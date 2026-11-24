# [cpp] override: a callee-destroyed by-value class parameter is still moved twice

Open after OVRMOVE (2026-11-19). A by-value C++ class parameter of a [cpp] struct override whose
C++ ABI has the CALLEE destroy it (`IsCxxParamDestroyedInCallee`) still takes the old path: the
generated wrapper forwards `static_cast<T&&>(param)` into a second by-value copy, or binds it as a
borrowed alias when the type cannot be forwarded (`CxxOverrideParamForwardsByValue` in LLVMBackend.h,
`isBorrowedCxxOverrideParam` in MainListener_Aggregates.cpp).

## Measured (macOS arm64, `[[clang::trivial_abi]]`)

`ovm.Ta` and `ovm.call_ta_xvalue` in Test/library/cpp_ovrmove.hpp; value/moves/copies/dtors:

```cflat
import cpp "library/cpp_ovrmove.hpp";
[cpp] struct D : ovm.Base { override int takeTa(ovm.Ta r) { return r.v + 1; } };
int main()
{
    D d = default;
    ovm.reset();
    int r = ovm.call_ta_xvalue(&d, 7);
    printf("%d %d %d %d\n", r, ovm.get_moves(), ovm.get_copies(), ovm.get_dtors());
    return 0;
}
```

- C++ dispatch: CFlat 208/2/0/3, clang++ -std=c++20 twin 108/1/0/2.
- Direct CFlat call `d.takeTa(move x)`: 108/1/0/1, same as clang.

## Why it is not a local fix (--no-opt IR of the shape above)

- The CFlat body destroys its parameter, because the type-level convention is callee-destroyed:
  `_takeTa$...(ptr %DTa__, ptr align 4 %r)` ends with `call ptr @_ZN3ovm2TaD1Ev(ptr %r)`. Direct
  CFlat calls depend on that destroy.
- The C++ twin owns its own `r` and destroys it on exit, and C++ cannot skip that:
  `_ZN12__cflat_user..6takeTaEN3ovm2TaE(ptr, i64)` calls `TaC1EOS0_` into a temp (the extra move),
  passes the temp to `__cflat_ovr_.._takeTa_0(ptr, i64)`, then calls `TaD1Ev(%3)` on its own `r`.
- So if the twin bound `r` by reference, the body would destroy `r` and then the twin would destroy
  it again. Removing the extra move needs a body entry that does NOT destroy the parameter (a drop
  flag or a second entry), or a CFlat caller-destroyed convention just for these override methods.
  Either way, every caller site that keys on the type-level convention must take a per-function
  exception: LLVMBackend_ControlFlowAndFunctions.cpp:886, LLVMBackend_Overloads.cpp 11733-11883 and
  12155, MainListener_Statements.cpp:8058, MainListener_Conditional.cpp 4744-4799,
  MainListener_Declarations.cpp:4527, the callee side at LLVMBackend_CodegenHelpers.cpp:197, plus
  function and method values.

## MS ABI scope (inferred from clang Sema, not measured: no Windows run)

Under the MS ABI, clang's `isParamDestroyedInCallee()` (CClangExtract.cpp:4392) is true for EVERY
record, trivial ones included (`areArgsDestroyedLeftToRightInCallee`). So on Windows OVRMOVE changes
nothing:
- Every non-trivial class keeps the double move through C++ dispatch and CFlat base dispatch.
- Classes with no listed copy/move ctor (Pod, Tc, Triv, and the non-const-copy Ncc) stay borrowed
  aliases. A direct CFlat call skips the copy, and a write in the body reaches the caller (Pod write
  10199 vs clang 10107).
- An [immovable] lvalue passed directly is still accepted by alias. Err leg in
  Test/errors/err_cpp_generated_final_base_relay.cb is gated `if const (!__WINDOWS__)`.

Legs pinned to the Windows values under `if const (__WINDOWS__)`: Test/test_cpp_interop.cb T331
33101/33102 and T340 34003 (Counted moves 1, dtors 2; clang 0, 1). Flip them with the fix.

Possible partial fix: forward only when `IsCxxParamDestroyedInCallee(t) && !HasTrivialCxxDtor(t)`,
since a trivially destructible parameter gives the callee nothing to destroy. That would bind Pod/Tc/
Triv on Windows like macOS, but it needs a Windows run of the reverse-thunk value load
(LLVMBackend_Interfaces.cpp:1697) and of the gated legs above.
