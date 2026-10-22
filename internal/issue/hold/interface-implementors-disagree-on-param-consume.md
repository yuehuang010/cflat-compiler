# p2: interface implementors that disagree on consuming a by-value string param double-free through the vtable (NEEDS RULING)

Pre-existing on master; found by the T378 review (2026-10-21). Repros: scratch/repro_keep/box1021/t378_rev/iface_dis_move_temp.cb,
iface_dis_mixed_temp.cb. Silent without ASan (bad=0), ASan double free with it, on master and after T378.

Shape: `interface I { string f(string s); }`, class A's `f` consumes `s` (`_ = move s;` or stores it), class B's `f` only borrows it;
a call through `I` with a temp argument. The call site cannot know which body runs. T378 resolves the call-site ownership from the
implementors' inferred summaries when they AGREE, and falls back to the declared contract when they disagree (LLVMBackend_WinRT.cpp ~3072).

## Ruling needed

(A) union of implementor summaries: the call site always transfers ownership through the vtable, and borrowing implementations free the
param when entered through the vtable (thunk or flag); or
(B) Rust prior art - the trait signature fixes ownership: an implementation that consumes a param the interface does not declare `move` is
refused at the implementation ("implementation of 'I.f' consumes 's'; declare it `move string s` in interface 'I'").
Main-session recommendation: (B) - ownership becomes part of the interface contract, callers need no whole-program view, and it matches
"borrow by default"; cost: existing interfaces whose implementors consume need a `move` on the interface param (count the sites first).
