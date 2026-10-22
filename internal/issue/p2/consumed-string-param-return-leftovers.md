# p2: string param consumed and/or stored by the callee - interface double free, method field-store UAF, lambda copy-arg leak

Pre-existing on master; found by the T360 review (2026-10-20). Cases: scratch/repro_keep/box1021/t360_rev/.
1. aba_iface: interface method `string f(string s) { _ = move s; return "q".copy(); }` called with a temp -> double free.
2. mixed_*: a free fn / method / lambda that consumes `s` on one path and returns it on another -> double free.
3. method_store: `string f(string s) { this.fld = s; return s; }` called with a temp -> heap-use-after-free.
4. lam_copyarg: a Lambda<string(string)> called with `"lit".copy()` leaks one allocation per call (200/200).
Root-cause area: call-site handling of by-value string params that the callee consumes or stores (IsConsumeInferredSink /
pendingOwnedStringTemps ledger, ApplyFuncPtrSinkTransfer, interface vtable path CallInterfaceMethod in LLVMBackend_WinRT.cpp).
Fix direction: one ownership answer per param kind (borrowed / consumed / stored / alias-returned) applied identically at direct,
method, interface and indirect call sites; T357/T360 adoption must compose with it.
